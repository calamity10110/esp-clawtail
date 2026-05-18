/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_thread.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "esp_err.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define THREAD_SYNC_MAX_OBJECTS 32
#define THREAD_SYNC_NAME_MAX 32
#define THREAD_SYNC_QUEUE_DEPTH_DEFAULT 8
#define THREAD_SYNC_QUEUE_DEPTH_MAX 32
#define THREAD_SYNC_QUEUE_MAX_BYTES_DEFAULT 2048
#define THREAD_SYNC_QUEUE_MAX_BYTES_MAX 4096
#define THREAD_SYNC_SEM_MAX_COUNT 255
#define THREAD_SYNC_WAIT_STEP_MS 50
#define THREAD_SYNC_JSON_MAX_DEPTH 64

typedef enum {
    THREAD_SYNC_TYPE_QUEUE = 0,
    THREAD_SYNC_TYPE_SEM,
    THREAD_SYNC_TYPE_LOCK,
} thread_sync_type_t;

typedef struct thread_sync_object {
    char *name;
    thread_sync_type_t type;
    union {
        QueueHandle_t queue;
        SemaphoreHandle_t sem;
        SemaphoreHandle_t mutex;
    } handle;
    size_t max_bytes;
    uint32_t waiter_count;
    TaskHandle_t lock_owner;
    struct thread_sync_object *next;
} thread_sync_object_t;

typedef struct {
    bool is_array;
    lua_Integer max_index;
    lua_Integer count;
} thread_sync_table_shape_t;

static thread_sync_object_t *s_objects;
static SemaphoreHandle_t s_registry_lock;
static size_t s_object_count;

static char *thread_sync_strdup(const char *value)
{
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);

    if (copy) {
        memcpy(copy, value, len);
    }
    return copy;
}

static esp_err_t thread_sync_ensure_lock(void)
{
    if (s_registry_lock) {
        return ESP_OK;
    }

    s_registry_lock = xSemaphoreCreateMutex();
    return s_registry_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void thread_sync_lock_registry(void)
{
    (void)thread_sync_ensure_lock();
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
}

static void thread_sync_unlock_registry(void)
{
    xSemaphoreGive(s_registry_lock);
}

static thread_sync_object_t *thread_sync_find_locked(const char *name)
{
    thread_sync_object_t *obj = s_objects;

    while (obj) {
        if (strcmp(obj->name, name) == 0) {
            return obj;
        }
        obj = obj->next;
    }
    return NULL;
}

static const char *thread_sync_check_name(lua_State *L, int index)
{
    size_t len = 0;
    const char *name = luaL_checklstring(L, index, &len);

    if (len == 0 || len > THREAD_SYNC_NAME_MAX) {
        luaL_error(L, "thread.sync: name length must be 1..%d", THREAD_SYNC_NAME_MAX);
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isalnum(ch) && ch != '_' && ch != '.' && ch != ':' && ch != '-') {
            luaL_error(L, "thread.sync: name contains invalid character");
        }
    }
    return name;
}

static uint32_t thread_sync_opt_uint(lua_State *L,
                                      int opts_idx,
                                      const char *field,
                                      uint32_t default_value,
                                      uint32_t min_value,
                                      uint32_t max_value)
{
    lua_Integer value = default_value;

    if (lua_istable(L, opts_idx)) {
        lua_getfield(L, opts_idx, field);
        if (!lua_isnil(L, -1)) {
            value = luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
    }

    if (value < (lua_Integer)min_value || value > (lua_Integer)max_value) {
        luaL_error(L, "thread.sync: option '%s' must be %u..%u",
                   field, (unsigned)min_value, (unsigned)max_value);
    }
    return (uint32_t)value;
}

static int thread_sync_push_nil_error(lua_State *L, const char *error)
{
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}

static int thread_sync_push_false_error(lua_State *L, const char *error)
{
    lua_pushboolean(L, false);
    lua_pushstring(L, error);
    return 2;
}

static thread_sync_object_t *thread_sync_acquire(lua_State *L,
                                                   const char *name,
                                                   thread_sync_type_t type)
{
    thread_sync_object_t *obj = NULL;

    thread_sync_lock_registry();
    obj = thread_sync_find_locked(name);
    if (!obj) {
        thread_sync_unlock_registry();
        return NULL;
    }
    if (obj->type != type) {
        thread_sync_unlock_registry();
        luaL_error(L, "thread.sync: object '%s' has a different type", name);
    }
    obj->waiter_count++;
    thread_sync_unlock_registry();
    return obj;
}

static void thread_sync_release(thread_sync_object_t *obj)
{
    thread_sync_lock_registry();
    if (obj->waiter_count > 0) {
        obj->waiter_count--;
    }
    thread_sync_unlock_registry();
}

static thread_sync_table_shape_t thread_sync_get_table_shape(lua_State *L, int index)
{
    thread_sync_table_shape_t shape = {
        .is_array = false,
        .max_index = 0,
        .count = 0,
    };

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2)) {
            lua_pop(L, 2);
            return shape;
        }

        lua_Integer key = lua_tointeger(L, -2);
        if (key <= 0) {
            lua_pop(L, 2);
            return shape;
        }
        if (key > shape.max_index) {
            shape.max_index = key;
        }
        shape.count++;
        lua_pop(L, 1);
    }

    shape.is_array = shape.count > 0 && shape.count == shape.max_index;
    return shape;
}

static cJSON *thread_sync_json_from_value(lua_State *L, int index, int depth);

static cJSON *thread_sync_json_from_array(lua_State *L,
                                           int index,
                                           lua_Integer count,
                                           int depth)
{
    cJSON *json = cJSON_CreateArray();

    if (!json) {
        return NULL;
    }

    index = lua_absindex(L, index);
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(L, index, i);
        cJSON *child = thread_sync_json_from_value(L, -1, depth + 1);
        lua_pop(L, 1);
        if (!child || !cJSON_AddItemToArray(json, child)) {
            cJSON_Delete(child);
            cJSON_Delete(json);
            return NULL;
        }
    }
    return json;
}

static cJSON *thread_sync_json_from_object(lua_State *L, int index, int depth)
{
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return NULL;
    }

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        char key_buf[32];
        const char *key = NULL;
        cJSON *child = thread_sync_json_from_value(L, -1, depth + 1);

        if (!child) {
            lua_pop(L, 1);
            cJSON_Delete(json);
            return NULL;
        }

        if (lua_type(L, -2) == LUA_TSTRING) {
            key = lua_tostring(L, -2);
        } else if (lua_isinteger(L, -2)) {
            snprintf(key_buf, sizeof(key_buf), "%lld", (long long)lua_tointeger(L, -2));
            key = key_buf;
        } else {
            cJSON_Delete(child);
            lua_pop(L, 1);
            cJSON_Delete(json);
            luaL_error(L, "thread.sync.queue_send: table keys must be strings or integers");
            return NULL;
        }

        if (!cJSON_AddItemToObject(json, key, child)) {
            cJSON_Delete(child);
            lua_pop(L, 1);
            cJSON_Delete(json);
            return NULL;
        }
        lua_pop(L, 1);
    }
    return json;
}

static cJSON *thread_sync_json_from_value(lua_State *L, int index, int depth)
{
    if (depth > THREAD_SYNC_JSON_MAX_DEPTH) {
        luaL_error(L, "thread.sync.queue_send: nested value too deep");
    }

    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, index));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, index));
    case LUA_TTABLE: {
        thread_sync_table_shape_t shape = thread_sync_get_table_shape(L, index);
        if (shape.is_array) {
            return thread_sync_json_from_array(L, index, shape.count, depth);
        }
        return thread_sync_json_from_object(L, index, depth);
    }
    case LUA_TNIL:
        luaL_error(L, "thread.sync.queue_send: nil values are not supported");
        return NULL;
    default:
        luaL_error(L, "thread.sync.queue_send: unsupported Lua type '%s'",
                   luaL_typename(L, index));
        return NULL;
    }
}

static void thread_sync_push_json_value(lua_State *L, const cJSON *item)
{
    cJSON *child = NULL;
    int index = 1;

    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
        return;
    }
    if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
        return;
    }
    if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
        return;
    }
    if (cJSON_IsArray(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            thread_sync_push_json_value(L, child);
            lua_rawseti(L, -2, index++);
        }
        return;
    }
    if (cJSON_IsObject(item)) {
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            thread_sync_push_json_value(L, child);
            lua_setfield(L, -2, child->string);
        }
        return;
    }
    lua_pushnil(L);
}

static char *thread_sync_encode_value(lua_State *L, int index, size_t max_bytes)
{
    cJSON *json = thread_sync_json_from_value(L, index, 0);
    char *payload = NULL;

    if (!json) {
        luaL_error(L, "thread.sync.queue_send: out of memory");
    }

    payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) {
        luaL_error(L, "thread.sync.queue_send: out of memory");
    }

    if (strlen(payload) > max_bytes) {
        cJSON_free(payload);
        luaL_error(L, "thread.sync.queue_send: encoded value exceeds max_bytes");
    }
    return payload;
}

static TickType_t thread_sync_ms_to_ticks(uint32_t ms)
{
    return ms == 0 ? 0 : pdMS_TO_TICKS(ms);
}

static uint32_t thread_sync_timeout_arg(lua_State *L, int index)
{
    lua_Integer timeout = luaL_optinteger(L, index, 0);

    if (timeout < 0) {
        luaL_error(L, "thread.sync: timeout must be >= 0");
    }
    if ((uint64_t)timeout > UINT32_MAX) {
        luaL_error(L, "thread.sync: timeout is too large");
    }
    return (uint32_t)timeout;
}

static int thread_sync_queue_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    int opts_idx = lua_gettop(L) >= 2 && !lua_isnil(L, 2) ? 2 : 0;
    uint32_t depth = 0;
    uint32_t max_bytes = 0;
    thread_sync_object_t *obj = NULL;

    if (opts_idx && !lua_istable(L, opts_idx)) {
        return luaL_error(L, "thread.sync.queue_create: opts must be a table");
    }
    depth = thread_sync_opt_uint(L, opts_idx, "depth",
                                  THREAD_SYNC_QUEUE_DEPTH_DEFAULT,
                                  1, THREAD_SYNC_QUEUE_DEPTH_MAX);
    max_bytes = thread_sync_opt_uint(L, opts_idx, "max_bytes",
                                      THREAD_SYNC_QUEUE_MAX_BYTES_DEFAULT,
                                      1, THREAD_SYNC_QUEUE_MAX_BYTES_MAX);

    obj = calloc(1, sizeof(*obj));
    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_QUEUE;
    obj->max_bytes = max_bytes;
    obj->handle.queue = xQueueCreate(depth, sizeof(char *));
    if (!obj->name || !obj->handle.queue) {
        if (obj->handle.queue) {
            vQueueDelete(obj->handle.queue);
        }
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        vQueueDelete(obj->handle.queue);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        vQueueDelete(obj->handle.queue);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_delete(lua_State *L, thread_sync_type_t type)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = NULL;
    thread_sync_object_t **link = NULL;

    thread_sync_lock_registry();
    link = &s_objects;
    while (*link && strcmp((*link)->name, name) != 0) {
        link = &(*link)->next;
    }
    obj = *link;
    if (!obj) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "not_found");
    }
    if (obj->type != type) {
        thread_sync_unlock_registry();
        return luaL_error(L, "thread.sync: object '%s' has a different type", name);
    }
    if (obj->waiter_count > 0) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }
    if (obj->type == THREAD_SYNC_TYPE_QUEUE && uxQueueMessagesWaiting(obj->handle.queue) > 0) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }
    if (obj->type == THREAD_SYNC_TYPE_LOCK && obj->lock_owner) {
        thread_sync_unlock_registry();
        return thread_sync_push_nil_error(L, "busy");
    }

    *link = obj->next;
    s_object_count--;
    thread_sync_unlock_registry();

    if (obj->type == THREAD_SYNC_TYPE_QUEUE) {
        vQueueDelete(obj->handle.queue);
    } else if (obj->type == THREAD_SYNC_TYPE_SEM) {
        vSemaphoreDelete(obj->handle.sem);
    } else {
        vSemaphoreDelete(obj->handle.mutex);
    }
    free(obj->name);
    free(obj);

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_queue_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_QUEUE);
}

static int thread_sync_queue_send(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 3);
    thread_sync_object_t *obj = NULL;
    char *payload = thread_sync_encode_value(L, 2, THREAD_SYNC_QUEUE_MAX_BYTES_MAX);
    BaseType_t ok = pdFALSE;
    uint32_t waited_ms = 0;

    obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_QUEUE);
    if (!obj) {
        cJSON_free(payload);
        return thread_sync_push_false_error(L, "not_found");
    }
    if (strlen(payload) > obj->max_bytes) {
        cJSON_free(payload);
        thread_sync_release(obj);
        return luaL_error(L, "thread.sync.queue_send: encoded value exceeds max_bytes");
    }
    do {
        uint32_t step_ms = 0;
        if (cap_lua_runtime_stop_requested(L)) {
            cJSON_free(payload);
            thread_sync_release(obj);
            return thread_sync_push_false_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > THREAD_SYNC_WAIT_STEP_MS ? THREAD_SYNC_WAIT_STEP_MS : remaining;
        }
        ok = xQueueSend(obj->handle.queue, &payload, thread_sync_ms_to_ticks(step_ms));
        if (ok == pdTRUE) {
            thread_sync_release(obj);
            lua_pushboolean(L, true);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    cJSON_free(payload);
    thread_sync_release(obj);
    return thread_sync_push_false_error(L, "timeout");
}

static int thread_sync_queue_recv(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 2);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_QUEUE);
    char *payload = NULL;
    BaseType_t ok = pdFALSE;
    uint32_t waited_ms = 0;

    if (!obj) {
        return thread_sync_push_nil_error(L, "not_found");
    }

    do {
        uint32_t step_ms = 0;
        if (cap_lua_runtime_stop_requested(L)) {
            thread_sync_release(obj);
            return thread_sync_push_nil_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > THREAD_SYNC_WAIT_STEP_MS ? THREAD_SYNC_WAIT_STEP_MS : remaining;
        }
        ok = xQueueReceive(obj->handle.queue, &payload, thread_sync_ms_to_ticks(step_ms));
        if (ok == pdTRUE) {
            cJSON *json = cJSON_Parse(payload);
            cJSON_free(payload);
            thread_sync_release(obj);
            if (!json) {
                return luaL_error(L, "thread.sync.queue_recv: invalid JSON payload");
            }
            thread_sync_push_json_value(L, json);
            cJSON_Delete(json);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    thread_sync_release(obj);
    return thread_sync_push_nil_error(L, "timeout");
}

static int thread_sync_sem_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    int opts_idx = lua_gettop(L) >= 2 && !lua_isnil(L, 2) ? 2 : 0;
    uint32_t max = 0;
    uint32_t initial = 0;
    thread_sync_object_t *obj = NULL;

    if (opts_idx && !lua_istable(L, opts_idx)) {
        return luaL_error(L, "thread.sync.sem_create: opts must be a table");
    }
    max = thread_sync_opt_uint(L, opts_idx, "max", 1, 1, THREAD_SYNC_SEM_MAX_COUNT);
    initial = thread_sync_opt_uint(L, opts_idx, "initial", 0, 0, max);

    obj = calloc(1, sizeof(*obj));
    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_SEM;
    obj->handle.sem = xSemaphoreCreateCounting(max, initial);
    if (!obj->name || !obj->handle.sem) {
        if (obj->handle.sem) {
            vSemaphoreDelete(obj->handle.sem);
        }
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        vSemaphoreDelete(obj->handle.sem);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        vSemaphoreDelete(obj->handle.sem);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_sem_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_SEM);
}

static int thread_sync_sem_give(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_SEM);
    BaseType_t ok = pdFALSE;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }
    ok = xSemaphoreGive(obj->handle.sem);
    thread_sync_release(obj);
    if (ok != pdTRUE) {
        return thread_sync_push_false_error(L, "full");
    }
    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_take_common(lua_State *L, thread_sync_type_t type)
{
    const char *name = thread_sync_check_name(L, 1);
    uint32_t timeout_ms = thread_sync_timeout_arg(L, 2);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, type);
    SemaphoreHandle_t handle = NULL;
    BaseType_t ok = pdFALSE;
    uint32_t waited_ms = 0;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }
    handle = type == THREAD_SYNC_TYPE_SEM ? obj->handle.sem : obj->handle.mutex;

    do {
        uint32_t step_ms = 0;
        if (cap_lua_runtime_stop_requested(L)) {
            thread_sync_release(obj);
            return thread_sync_push_false_error(L, "stopped");
        }
        if (timeout_ms > waited_ms) {
            uint32_t remaining = timeout_ms - waited_ms;
            step_ms = remaining > THREAD_SYNC_WAIT_STEP_MS ? THREAD_SYNC_WAIT_STEP_MS : remaining;
        }
        ok = xSemaphoreTake(handle, thread_sync_ms_to_ticks(step_ms));
        if (ok == pdTRUE) {
            if (type == THREAD_SYNC_TYPE_LOCK) {
                thread_sync_lock_registry();
                obj->lock_owner = xTaskGetCurrentTaskHandle();
                thread_sync_unlock_registry();
            }
            thread_sync_release(obj);
            lua_pushboolean(L, true);
            return 1;
        }
        waited_ms += step_ms;
    } while (timeout_ms > waited_ms);

    thread_sync_release(obj);
    return thread_sync_push_false_error(L, "timeout");
}

static int thread_sync_sem_take(lua_State *L)
{
    return thread_sync_take_common(L, THREAD_SYNC_TYPE_SEM);
}

static int thread_sync_lock_create(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = calloc(1, sizeof(*obj));

    if (!obj) {
        return thread_sync_push_nil_error(L, "no_mem");
    }
    obj->name = thread_sync_strdup(name);
    obj->type = THREAD_SYNC_TYPE_LOCK;
    obj->handle.mutex = xSemaphoreCreateMutex();
    if (!obj->name || !obj->handle.mutex) {
        if (obj->handle.mutex) {
            vSemaphoreDelete(obj->handle.mutex);
        }
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "no_mem");
    }

    thread_sync_lock_registry();
    if (thread_sync_find_locked(name)) {
        thread_sync_unlock_registry();
        vSemaphoreDelete(obj->handle.mutex);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "exists");
    }
    if (s_object_count >= THREAD_SYNC_MAX_OBJECTS) {
        thread_sync_unlock_registry();
        vSemaphoreDelete(obj->handle.mutex);
        free(obj->name);
        free(obj);
        return thread_sync_push_nil_error(L, "limit");
    }
    obj->next = s_objects;
    s_objects = obj;
    s_object_count++;
    thread_sync_unlock_registry();

    lua_pushboolean(L, true);
    return 1;
}

static int thread_sync_lock_delete(lua_State *L)
{
    return thread_sync_delete(L, THREAD_SYNC_TYPE_LOCK);
}

static int thread_sync_lock_take(lua_State *L)
{
    return thread_sync_take_common(L, THREAD_SYNC_TYPE_LOCK);
}

static int thread_sync_unlock(lua_State *L)
{
    const char *name = thread_sync_check_name(L, 1);
    thread_sync_object_t *obj = thread_sync_acquire(L, name, THREAD_SYNC_TYPE_LOCK);
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    BaseType_t ok = pdFALSE;

    if (!obj) {
        return thread_sync_push_false_error(L, "not_found");
    }

    thread_sync_lock_registry();
    if (obj->lock_owner != current) {
        thread_sync_unlock_registry();
        thread_sync_release(obj);
        return thread_sync_push_false_error(L, "not_owner");
    }
    obj->lock_owner = NULL;
    thread_sync_unlock_registry();

    ok = xSemaphoreGive(obj->handle.mutex);
    thread_sync_release(obj);
    if (ok != pdTRUE) {
        return thread_sync_push_false_error(L, "unlock_failed");
    }
    lua_pushboolean(L, true);
    return 1;
}

int lua_module_thread_push_sync(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"queue_create", thread_sync_queue_create},
        {"queue_send", thread_sync_queue_send},
        {"queue_recv", thread_sync_queue_recv},
        {"queue_delete", thread_sync_queue_delete},
        {"sem_create", thread_sync_sem_create},
        {"sem_give", thread_sync_sem_give},
        {"sem_take", thread_sync_sem_take},
        {"sem_delete", thread_sync_sem_delete},
        {"lock_create", thread_sync_lock_create},
        {"lock", thread_sync_lock_take},
        {"unlock", thread_sync_unlock},
        {"lock_delete", thread_sync_lock_delete},
        {NULL, NULL},
    };

    if (thread_sync_ensure_lock() != ESP_OK) {
        luaL_error(L, "thread.sync: failed to create registry lock");
    }

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t thread_sync_init(void)
{
    return thread_sync_ensure_lock();
}
