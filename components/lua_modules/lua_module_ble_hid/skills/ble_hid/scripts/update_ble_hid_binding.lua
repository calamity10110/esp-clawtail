local settings = require("ble_hid_settings")
local actions = require("ble_hid_actions")

local function fail(message)
    error("[update_ble_hid_binding] " .. tostring(message))
end

local function value(name, fallback)
    if type(args) == "table" and args[name] ~= nil then
        return args[name]
    end
    return fallback
end

local op = value("op", "update")
if op == "reset" then
    local ok, err = settings.reset()
    if not ok then
        fail(err)
    end
    print("ble_hid bindings reset to defaults")
    return
end
if op ~= "update" then
    fail("op must be update or reset")
end

local function build_input_event()
    local input_event = value("input_event")
    if type(input_event) == "string" and input_event ~= "" then
        return input_event
    end

    local source = value("source", "button")
    local input_id = value("input_id", "main")
    local event = value("event", "single_click")
    if type(source) ~= "string" or source == "" then
        fail("source must be a non-empty string")
    end
    if type(input_id) ~= "string" or input_id == "" then
        fail("input_id must be a non-empty string")
    end
    if type(event) ~= "string" or event == "" then
        fail("event must be a non-empty string")
    end
    return source .. ":" .. input_id .. ":" .. event
end

local function build_action()
    local action
    local normalized
    local err

    if type(args) == "table" and type(args.action) == "table" then
        action = args.action
    else
        local action_type = value("type", "media")
        if type(action_type) ~= "string" or action_type == "" then
            fail("type must be a non-empty string")
        end

        action = {
            type = action_type,
            key = value("key"),
            gesture = value("gesture"),
            keys = value("keys"),
            text = value("text"),
            button = value("button"),
            dx = value("dx"),
            dy = value("dy"),
            vertical = value("vertical"),
            horizontal = value("horizontal"),
            scale = value("scale"),
        }
    end

    normalized, err = actions.validate_action(action)
    if not normalized then
        fail(err)
    end
    return normalized
end

local input_event = build_input_event()
local action = build_action()
local ok, err = settings.update_binding(input_event, action)
if not ok then
    fail(err)
end

print("updated ble_hid binding input_event=" .. input_event .. " action_type=" .. action.type)
