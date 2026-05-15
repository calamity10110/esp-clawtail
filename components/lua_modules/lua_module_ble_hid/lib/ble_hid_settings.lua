local M = {}
local actions = require("ble_hid_actions")

M.SCHEMA_VERSION = 3
M.DEFAULT_PATH = "/fatfs/scripts/config/ble_hid_bindings.lua"

local cache = nil
local cache_path = nil

local function try_storage()
    local ok, storage = pcall(require, "storage")
    if ok then
        return storage
    end
    return nil
end

local function read_file(path)
    local storage = try_storage()
    if storage then
        local ok, data = pcall(storage.read_file, path)
        if ok then
            return data
        end
        return nil, data
    end

    local file, err = io.open(path, "r")
    if not file then
        return nil, err
    end
    local data = file:read("*a")
    file:close()
    return data
end

local function ensure_config_dir()
    local storage = try_storage()
    if not storage then
        return true
    end

    local root = storage.get_root_dir and storage.get_root_dir() or "/fatfs"
    local scripts_dir = storage.join_path(root, "scripts")
    local config_dir = storage.join_path(root, "scripts", "config")

    if not storage.exists(scripts_dir) then
        local ok, err = pcall(storage.mkdir, scripts_dir)
        if not ok then
            return nil, err
        end
    end
    if not storage.exists(config_dir) then
        local ok, err = pcall(storage.mkdir, config_dir)
        if not ok then
            return nil, err
        end
    end
    return true
end

local function write_file(path, content)
    local ok, err = ensure_config_dir()
    if not ok then
        return nil, err
    end

    local storage = try_storage()
    if storage then
        ok, err = pcall(storage.write_file, path, content)
        if ok then
            return true
        end
        return nil, err
    end

    local file
    file, err = io.open(path, "w")
    if not file then
        return nil, err
    end
    file:write(content)
    file:close()
    return true
end

local function deep_copy(value)
    if type(value) ~= "table" then
        return value
    end
    local copy = {}
    for key, item in pairs(value) do
        copy[deep_copy(key)] = deep_copy(item)
    end
    return copy
end

local function default_config()
    return {
        schema_version = M.SCHEMA_VERSION,
        inputs = {
            main = {
                type = "button",
                gpio = 0,
                active_level = 0,
            },
        },
        bindings = actions.default_bindings(),
    }
end

local function is_identifier(value)
    return type(value) == "string" and value:match("^[%a_][%w_]*$") ~= nil
end

local function is_array(value)
    if type(value) ~= "table" then
        return false
    end
    local count = 0
    for key, _ in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
            return false
        end
        if key > count then
            count = key
        end
    end
    for index = 1, count do
        if value[index] == nil then
            return false
        end
    end
    return count > 0
end

local function serialize_value(value, indent)
    indent = indent or 0
    local pad = string.rep(" ", indent)
    local child_pad = string.rep(" ", indent + 4)

    if type(value) == "string" then
        return string.format("%q", value)
    end
    if type(value) == "number" or type(value) == "boolean" then
        return tostring(value)
    end
    if type(value) ~= "table" then
        return "nil"
    end

    local parts = { "{" }
    if is_array(value) then
        for index = 1, #value do
            parts[#parts + 1] = child_pad .. serialize_value(value[index], indent + 4) .. ","
        end
    else
        local keys = {}
        for key, _ in pairs(value) do
            keys[#keys + 1] = key
        end
        table.sort(keys, function(a, b)
            return tostring(a) < tostring(b)
        end)
        for _, key in ipairs(keys) do
            local rendered_key
            if is_identifier(key) then
                rendered_key = key
            else
                rendered_key = "[" .. serialize_value(key, 0) .. "]"
            end
            parts[#parts + 1] = child_pad .. rendered_key .. " = " .. serialize_value(value[key], indent + 4) .. ","
        end
    end
    parts[#parts + 1] = pad .. "}"
    return table.concat(parts, "\n")
end

local function normalize_config(config)
    if type(config) ~= "table" then
        config = default_config()
    end
    config.schema_version = M.SCHEMA_VERSION
    config.inputs = type(config.inputs) == "table" and config.inputs or default_config().inputs
    config.bindings = type(config.bindings) == "table" and config.bindings or default_config().bindings
    for input_event, action in pairs(config.bindings or {}) do
        if type(input_event) == "string" then
            local normalized = actions.normalize_action(action)
            if normalized then
                config.bindings[input_event] = normalized
            else
                config.bindings[input_event] = nil
            end
        end
    end
    return config
end

function M.default_config()
    return default_config()
end

function M.serialize(config)
    return "return " .. serialize_value(normalize_config(deep_copy(config)), 0) .. "\n"
end

function M.load(path, force)
    path = path or M.DEFAULT_PATH
    if cache and cache_path == path and not force then
        return cache
    end

    local content = read_file(path)
    if not content or content == "" then
        local config = default_config()
        M.save(config, path)
        cache = config
        cache_path = path
        return cache
    end

    local chunk, load_err = load(content, "@" .. path, "t", {})
    if not chunk then
        return nil, load_err
    end

    local ok, config = pcall(chunk)
    if not ok then
        return nil, config
    end

    cache = normalize_config(config)
    cache_path = path
    return cache
end

function M.reload(path)
    return M.load(path, true)
end

function M.save(config, path)
    path = path or M.DEFAULT_PATH
    local normalized = normalize_config(deep_copy(config))
    local ok, err = write_file(path, M.serialize(normalized))
    if not ok then
        return nil, err
    end
    cache = normalized
    cache_path = path
    return true
end

function M.reset(path)
    return M.save(default_config(), path)
end

function M.get_binding(input_event, path)
    if type(input_event) ~= "string" or input_event == "" then
        return nil, "input_event must be a non-empty string"
    end
    local config, err = M.load(path, true)
    if not config then
        return nil, err
    end
    return config.bindings and config.bindings[input_event] or nil
end

function M.update_binding(input_event, action, path)
    if type(input_event) ~= "string" or input_event == "" then
        return nil, "input_event must be a non-empty string"
    end
    if type(action) ~= "table" then
        return nil, "action must be a table"
    end

    local normalized_action, validation_err = actions.validate_action(action)
    if not normalized_action then
        return nil, validation_err
    end

    local config, err = M.load(path, true)
    if not config then
        return nil, err
    end
    config.bindings = config.bindings or {}
    config.bindings[input_event] = deep_copy(normalized_action)
    return M.save(config, path)
end

return M
