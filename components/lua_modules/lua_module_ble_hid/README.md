# lua_module_ble_hid

## Overview

`lua_module_ble_hid` is a thin BLE HID profile adapter for ESP-Claw. It keeps
only the product-facing HID surface in this component:

- one combined HID report map for Consumer Control, Keyboard, and Mouse
- small report encoders that create report bytes
- ESP-IDF `esp_hidd` glue
- Lua bindings exposed as `require("ble_hid")`
- BLE HID skill scripts for agent-driven runtime operations

The BLE HID GATT service, HID characteristics, CCCD handling, protocol mode,
control point, and notification transport are owned by ESP-IDF `esp_hidd`.

## Ownership

Runtime ownership is:

```text
Lua behavior scripts
-> ble_hid.* Lua binding
-> esp_hidd_dev_input_set()
-> ESP-IDF esp_hidd
-> ESP-IDF NimBLE / Bluetooth controller
```

`lua_module_ble_hid` owns its BLE HID runtime directly. It initializes the
Bluetooth controller, NimBLE host, HID GATT service, security, connection state,
and HID advertising without depending on `lua_module_ble`.

Do not start the generic `lua_module_ble` advertising while BLE HID is running.
The two modules can be compiled into the same firmware, but they must not both
try to own the active BLE host / advertising session at runtime.

## C Implementation

The C side is intentionally contained in:

```text
src/lua_module_ble_hid.c
src/lua_module_ble_hid.h
```

`lua_module_ble_hid.c` contains static report map bytes, static key/action lookup
tables, minimal report encoding, and the `esp_hidd` device handle. There are no
separate HID transport, HID runtime dispatcher, profile registry, or HID service
component files.

## Lua API

```lua
local ble_hid = require("ble_hid")

ble_hid.init()
ble_hid.start({ name = "esp-claw-hid" })

ble_hid.media("play_pause")
ble_hid.media("volume_up")
ble_hid.media("volume_down")
ble_hid.media("next_track")
ble_hid.media("previous_track")
ble_hid.media("mute")

ble_hid.key("ENTER")
ble_hid.combo("CTRL", "C")
ble_hid.text("hello")

ble_hid.mouse_move(20, 0)
ble_hid.mouse_button("left", "click")
ble_hid.mouse_scroll(-3, 0)

ble_hid.release_all()
ble_hid.stop()
ble_hid.deinit()
```

`ble_hid.text(text)` is ASCII keyboard simulation on a standard US keyboard layout.
It supports printable ASCII plus newline and tab. It does not promise Unicode,
IME behavior, emoji, dead keys, or non-US layout correctness.

## Runtime Scripts

Agent-facing runtime scripts live under `/fatfs/skills/ble_hid/scripts/` on the device:

```text
/fatfs/skills/ble_hid/scripts/start_ble_hid.lua
/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua
/fatfs/skills/ble_hid/scripts/update_ble_hid_binding.lua
```

Roles:

- `start_ble_hid.lua`: idempotently initialize BLE HID and start HID advertising
- `send_ble_hid_action.lua`: send one media, keyboard, or mouse action
- `update_ble_hid_binding.lua`: update or reset local button-to-action bindings

Reusable internal libraries live under `lib/`:

```text
lib/ble_hid_actions.lua
lib/ble_hid_settings.lua
```

These are internal modules, not agent entry points.

## Local Button Rules

`lua_module_ble_hid` does not listen to physical buttons. Local input handling is
an application-layer concern. In `edge_agent`, app skills under
`application/edge_agent/main/skills/ble_hid_input/` require both `button` and
`ble_hid` to listen for physical input and dispatch mapped HID actions.

The shared binding configuration lives at:

```text
/fatfs/scripts/config/ble_hid_bindings.lua
```

The default input is:

```lua
inputs = {
    main = {
        type = "button",
        gpio = 0,
        active_level = 0,
    },
}
```

Default bindings:

```lua
bindings = {
    ["button:main:single_click"] = { type = "media", key = "play_pause", gesture = "single" },
    ["button:main:double_click"] = { type = "media", key = "next_track", gesture = "single" },
    ["button:main:long_press_start"] = { type = "media", key = "volume_up", gesture = "single" },
}
```

Supported local events are `single_click`, `double_click`, `long_press_start`,
and `long_press_up`. Supported action types are `media`, `keyboard_key`,
`keyboard_text`, `mouse_button`, `mouse_move`, and `mouse_scroll`.

Use `update_ble_hid_binding.lua` to update the file. BLE HID is a composite
device: media, keyboard, and mouse reports are all available at the same time,
and each binding action decides which report type is sent. The next
application-layer button dispatch will use the latest saved configuration.

Application-layer listener:

```text
/fatfs/skills/ble_hid_input/scripts/listen_ble_hid_input.lua
```

The `edge_agent` application starts this listener from its
`startup/boot_completed` router rule. The listener reads the configured
`inputs.main` GPIO and active level from `/fatfs/scripts/config/ble_hid_bindings.lua`
unless explicit args are provided.

## HID Reports

The report map defines three input reports in one HID service:

- Consumer Control: Report ID 1, 1 byte
- Keyboard: Report ID 2, 8 bytes (`modifier + reserved + 6 keycodes`)
- Mouse: Report ID 3, 5 bytes (`buttons + x + y + wheel + horizontal pan`)

Application code sends report payload bytes through:

```c
esp_hidd_dev_input_set(dev, 0, report_id, data, len);
```

The report payload does not include the report ID; `esp_hidd` handles the BLE HID
service and report characteristic transport.

## Configuration

Enable:

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_APP_CLAW_LUA_MODULE_BLE_HID=y
```

For HID-focused builds, disable `CONFIG_APP_CLAW_BLE_TEST_SERVICE` and avoid
starting `lua_module_ble/test/test_ble.lua`; that script is for the generic
`0xFFF0/0xFFF1` BLE test service, not HID.

## Verification

Runtime entry point from the ESP-Claw Lua console:

```text
lua --run --path /fatfs/skills/ble_hid/scripts/start_ble_hid.lua --timeout-ms 10000
```

Developer-only HID verification after flashing `edge_agent`:

```text
lua --run --path builtin/test/test_ble_hid.lua --timeout-ms 60000
```

Do not use scripts under `test/` as runtime entry points for agent behavior; they
exist only for developer bring-up and manual debugging.

Pair from the operating system Bluetooth settings. HID hosts normally discover
the HID service and subscribe to reports through the system Bluetooth stack.
