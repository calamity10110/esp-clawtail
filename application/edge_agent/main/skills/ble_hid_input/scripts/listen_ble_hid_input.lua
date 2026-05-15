local button = require("button")
local actions = require("ble_hid_actions")
local settings = require("ble_hid_settings")

local cfg = type(args) == "table" and args or {}
local input_id = cfg.input_id or "main"
local settings_config, settings_err = settings.load()
local input_cfg = {}
if settings_config and type(settings_config.inputs) == "table" and type(settings_config.inputs[input_id]) == "table" then
    input_cfg = settings_config.inputs[input_id]
elseif settings_err then
    print("[ble_hid_input] load settings failed, using fallback input config: " .. tostring(settings_err))
end

local gpio = cfg.gpio
if gpio == nil then
    gpio = input_cfg.gpio or 0
end
local active_level = cfg.active_level
if active_level == nil then
    active_level = input_cfg.active_level or 0
end
local poll_ms = cfg.poll_ms or 20
local idle_log_ms = cfg.idle_log_ms or 30000
local events = cfg.events or {
    "single_click",
    "double_click",
    "long_press_start",
    "long_press_up",
}

local delay_ok, delay = pcall(require, "delay")

local function sleep_ms(ms)
    if delay_ok and delay and delay.delay_ms then
        delay.delay_ms(ms)
    else
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do
        end
    end
end

local function build_input_event(event_name)
    return "button:" .. input_id .. ":" .. event_name
end

local function dispatch_input_event(event_name)
    local input_event = build_input_event(event_name)
    local action, err = settings.get_binding(input_event)

    if err then
        print("[ble_hid_input] get_binding failed input_event=" .. input_event .. " err=" .. tostring(err))
        return
    end
    if not action then
        print("[ble_hid_input] no binding for input_event=" .. input_event)
        return
    end

    local ran
    local ok
    ran, ok, err = pcall(actions.run, action)
    if not ran then
        print("[ble_hid_input] action error input_event=" .. input_event .. " err=" .. tostring(ok))
        return
    end
    if not ok then
        print("[ble_hid_input] action failed input_event=" .. input_event .. " err=" .. tostring(err))
        return
    end
    print("[ble_hid_input] action sent input_event=" .. input_event .. " type=" .. tostring(action.type))
end

local handle, err = button.new(gpio, active_level)
if not handle then
    error("[ble_hid_input] button.new failed: " .. tostring(err))
end

for _, event_name in ipairs(events) do
    local ok
    ok, err = button.on(handle, event_name, function(evt)
        dispatch_input_event(evt.event or event_name)
    end)
    if not ok then
        pcall(button.close, handle)
        error("[ble_hid_input] button.on failed event=" .. tostring(event_name) .. " err=" .. tostring(err))
    end
end

print("[ble_hid_input] listening gpio=" .. tostring(gpio)
    .. " active_level=" .. tostring(active_level)
    .. " input_id=" .. tostring(input_id))

local last_idle_log = os.clock()
while true do
    button.dispatch()
    if idle_log_ms > 0 and ((os.clock() - last_idle_log) * 1000) >= idle_log_ms then
        print("[ble_hid_input] alive input_id=" .. tostring(input_id))
        last_idle_log = os.clock()
    end
    sleep_ms(poll_ms)
end
