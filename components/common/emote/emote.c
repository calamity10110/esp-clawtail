/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "emote.h"

#include <stdint.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_board_manager_includes.h"
#include "expression_emote.h"
#include "freertos/FreeRTOS.h"
#include "gfx.h"
#include "display_arbiter.h"

static const char *TAG = "app_emote";

#define EMOTE_ASSETS_PARTITION "emote"

static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static int s_lcd_width;
static int s_lcd_height;
static int s_lcd_bits_per_pixel;
#if CONFIG_EMOTE_RGB888_SUPPORT
static bool s_lcd_rgb_order = true;
static uint8_t *s_rgb888_flush_buf;
static size_t s_rgb888_flush_buf_pixels;
#endif
static emote_handle_t s_emote_handle;

static bool emote_should_swap_color(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    if (lcd_cfg->bits_per_pixel == 24) {
        return false;
    }

    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 || strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0 || strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return false;
    }

    return true;
}

#if CONFIG_EMOTE_RGB888_SUPPORT
static void emote_rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
        const uint16_t color = src[i];
        const uint8_t r = (uint8_t)((((color >> 11) & 0x1F) * 255U) / 31U);
        const uint8_t g = (uint8_t)((((color >> 5) & 0x3F) * 255U) / 63U);
        const uint8_t b = (uint8_t)(((color & 0x1F) * 255U) / 31U);

        // Match the LCD panel RGB element order while expanding from the internal RGB565 render buffer.
        if (s_lcd_rgb_order) {
            dst[i * 3] = r;
            dst[i * 3 + 1] = g;
            dst[i * 3 + 2] = b;
        } else {
            dst[i * 3] = b;
            dst[i * 3 + 1] = g;
            dst[i * 3 + 2] = r;
        }
    }
}
#endif

static esp_err_t emote_prepare_rgb888_flush_buffer(size_t buf_pixels)
{
    if (s_lcd_bits_per_pixel != 24) {
        return ESP_OK;
    }

#if CONFIG_EMOTE_RGB888_SUPPORT
    if (s_rgb888_flush_buf != NULL && s_rgb888_flush_buf_pixels >= buf_pixels) {
        return ESP_OK;
    }

    if (s_rgb888_flush_buf != NULL) {
        heap_caps_free(s_rgb888_flush_buf);
        s_rgb888_flush_buf = NULL;
        s_rgb888_flush_buf_pixels = 0;
    }

    if (buf_pixels > SIZE_MAX / 3) {
        ESP_LOGE(TAG, "RGB888 flush buffer size overflow: %u pixels", (unsigned)buf_pixels);
        return ESP_ERR_INVALID_SIZE;
    }

    s_rgb888_flush_buf = (uint8_t *)heap_caps_malloc(buf_pixels * 3, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_rgb888_flush_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate RGB888 flush buffer: %u pixels", (unsigned)buf_pixels);
        return ESP_ERR_NO_MEM;
    }

    s_rgb888_flush_buf_pixels = buf_pixels;
#else
    ESP_LOGE(TAG, "RGB888 LCD panel detected but EMOTE_RGB888_SUPPORT is disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    return ESP_OK;
}

static void emote_on_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (owner != DISPLAY_ARBITER_OWNER_EMOTE || !s_emote_handle) {
        return;
    }

    esp_err_t err = emote_notify_all_refresh(s_emote_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh after owner switch failed: %s", esp_err_to_name(err));
    }
}

static void emote_flush_callback(int x_start, int y_start, int x_end, int y_end,
                                 const void *data, emote_handle_t handle)
{
    if (!s_panel_handle || !display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        if (handle) {
            emote_notify_flush_finished(handle);
        }
        return;
    }

    if (data == NULL) {
        ESP_LOGE(TAG, "flush data is NULL");
        if (handle) {
            emote_notify_flush_finished(handle);
        }
        return;
    }

    const void *flush_data = data;
#if CONFIG_EMOTE_RGB888_SUPPORT
    const size_t pixel_count = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);
    if (s_lcd_bits_per_pixel == 24) {
        if (s_rgb888_flush_buf == NULL || pixel_count > s_rgb888_flush_buf_pixels) {
            ESP_LOGE(TAG, "RGB888 flush buffer too small: need %u pixels, have %u pixels",
                     (unsigned)pixel_count, (unsigned)s_rgb888_flush_buf_pixels);
            if (handle) {
                emote_notify_flush_finished(handle);
            }
            return;
        }
        emote_rgb565_to_rgb888((const uint16_t *)data, s_rgb888_flush_buf, pixel_count);
        flush_data = s_rgb888_flush_buf;
    }
#endif

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, flush_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
    }

    if (handle) {
        emote_notify_flush_finished(handle);
    }
}

static void emote_update_callback(gfx_disp_event_t event, const void *obj,
                                  emote_handle_t handle)
{
    if (!handle) {
        return;
    }

    gfx_obj_t *wait_obj = emote_get_obj_by_name(handle, EMT_DEF_ELEM_EMERG_DLG);
    if (wait_obj == obj && event == GFX_DISP_EVENT_ALL_FRAME_DONE) {
        ESP_LOGI(TAG, "Emergency dialog finished");
    }
}

static esp_err_t emote_load_board_display(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        return err;
    }

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;

    ESP_RETURN_ON_FALSE(lcd_handles && lcd_cfg && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG, "display_lcd handle/config is NULL");

    s_panel_handle = lcd_handles->panel_handle;
    s_io_handle = lcd_handles->io_handle;
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    s_lcd_bits_per_pixel = lcd_cfg->bits_per_pixel;
#if CONFIG_EMOTE_RGB888_SUPPORT
    s_lcd_rgb_order = lcd_cfg->rgb_ele_order != LCD_RGB_ELEMENT_ORDER_BGR;
#endif
    return ESP_OK;
#endif
}

static emote_config_t emote_get_default_config(void)
{
    void *lcd_config = NULL;
    bool swap = true;
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config) == ESP_OK) {
        swap = emote_should_swap_color((const dev_display_lcd_config_t *)lcd_config);
    }

    emote_config_t config = {
        .flags = {
            .swap = swap,
            .double_buffer = true,
            .buff_dma = true,
        },
        .gfx_emote = {
            .h_res = s_lcd_width,
            .v_res = s_lcd_height,
            .fps = 10,
        },
        .buffers = {
            .buf_pixels = (size_t)s_lcd_width * 16,
        },
        .task = {
            .task_priority = 3,
            .task_stack = 12 * 1024,
            .task_affinity = -1,
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .task_stack_in_ext = true,
#else
            .task_stack_in_ext = false,
#endif
        },
        .flush_cb = emote_flush_callback,
        .update_cb = emote_update_callback,
    };

    return config;
}

static esp_err_t emote_apply(const char *idle, const char *msg)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_SYS, msg), TAG, "set emote message failed");
    ESP_RETURN_ON_ERROR(emote_set_anim_emoji(s_emote_handle, idle), TAG, "set emote idle animation failed");

    if (display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)) {
        ESP_RETURN_ON_ERROR(emote_notify_all_refresh(s_emote_handle), TAG, "refresh emote display failed");
    }

    return ESP_OK;
}

esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "emote handle is NULL");

    const bool ap_present = (ap_ssid != NULL && ap_ssid[0] != '\0');
    const char *idle = sta_connected ? "swim" : "offline";

    char msg[96];
    if (sta_connected && ap_present) {
        snprintf(msg, sizeof(msg), "Online * AP: %s", ap_ssid);
    } else if (sta_connected) {
        snprintf(msg, sizeof(msg), "Wi-Fi connected");
    } else if (ap_present) {
        snprintf(msg, sizeof(msg), "Setup WiFi: %s", ap_ssid);
    } else {
        snprintf(msg, sizeof(msg), "Wi-Fi offline");
    }

    return emote_apply(idle, msg);
}

static void emote_cleanup(void)
{
    if (s_emote_handle) {
        emote_deinit(s_emote_handle);
        s_emote_handle = NULL;
    }
#if CONFIG_EMOTE_RGB888_SUPPORT
    if (s_rgb888_flush_buf) {
        heap_caps_free(s_rgb888_flush_buf);
        s_rgb888_flush_buf = NULL;
        s_rgb888_flush_buf_pixels = 0;
    }
#endif
    display_arbiter_set_owner_changed_callback(NULL, NULL);
}

static esp_err_t emote_init_internal(void)
{
    emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = {
            .partition_label = EMOTE_ASSETS_PARTITION,
        },
        .flags = {
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .mmap_enable = false,
#else
            .mmap_enable = true,
#endif
        },
    };

    if (s_emote_handle) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(emote_load_board_display(), TAG, "Failed to get board display handles");

    emote_config_t config = emote_get_default_config();
    ESP_RETURN_ON_ERROR(emote_prepare_rgb888_flush_buffer(config.buffers.buf_pixels), TAG, "prepare RGB888 flush buffer failed");
    ESP_RETURN_ON_ERROR(display_arbiter_set_owner_changed_callback(emote_on_owner_changed, NULL), TAG, "register display owner callback failed");
    s_emote_handle = emote_init(&config);
    if (!s_emote_handle || !emote_is_initialized(s_emote_handle)) {
        emote_cleanup();
        return ESP_FAIL;
    }

    esp_err_t err = emote_mount_and_load_assets(s_emote_handle, &data);
    if (err != ESP_OK) {
        emote_cleanup();
        return err;
    }

    return emote_set_network_status(false, NULL);
}

esp_err_t emote_start(void)
{
    esp_err_t err = emote_init_internal();
    if (err != ESP_OK) {
        emote_cleanup();
    }
    return err;
}
