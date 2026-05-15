local actions = require("ble_hid_actions")
local settings = require("ble_hid_settings")

local function build_input_event(value)
    if type(value) ~= "table" then
        return nil
    end
    if type(value.input_event) == "string" and value.input_event ~= "" then
        return value.input_event
    end
    if type(value.source) == "string" and value.source ~= "" and
        type(value.input_id) == "string" and value.input_id ~= "" and
        type(value.event) == "string" and value.event ~= "" then
        return value.source .. ":" .. value.input_id .. ":" .. value.event
    end
    return nil
end

local input_event = build_input_event(args)
local action
local err

if input_event then
    action, err = settings.get_binding(input_event)
    if err then
        error("[send_ble_hid_action] " .. tostring(err))
    end
    if not action then
        print("ble_hid no binding for input_event=" .. input_event)
        return
    end
else
    action = type(args) == "table" and type(args.action) == "table" and args.action or args
end

local ok
ok, err = actions.run(action)
if not ok then
    error("[send_ble_hid_action] " .. tostring(err))
end

print("ble_hid action sent type=" .. tostring(action and action.type)
    .. (input_event and (" input_event=" .. input_event) or ""))
