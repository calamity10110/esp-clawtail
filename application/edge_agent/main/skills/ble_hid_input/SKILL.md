---
{
  "name": "ble_hid_input",
  "description": "Application-layer BLE HID input bridge. Listen to local buttons and dispatch configured BLE HID actions.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# BLE HID Input

Use this skill when the user wants local physical buttons to trigger BLE HID
media, keyboard, or mouse actions.

Run `/fatfs/skills/ble_hid_input/scripts/listen_ble_hid_input.lua` with
`lua_run_script_async` as the long-running application-layer listener. Do not use
the relative path `scripts/listen_ble_hid_input.lua`; relative Lua paths resolve
under `/fatfs/scripts`, while this listener is a skill script under
`/fatfs/skills`.

This script owns the `button` dependency and dispatches into the BLE HID action
mapping. `lua_module_ble_hid` itself should remain a BLE HID capability module
and should not listen to buttons.

Typical args:

```json
{
  "gpio": 0,
  "active_level": 0,
  "input_id": "main"
}
```

If args are omitted, the listener reads `inputs.<input_id>` from the BLE HID
settings file. The default config uses `button:main:single_click` mapped to
`media/play_pause`.

The `edge_agent` startup router rule starts this listener automatically after
`app_claw` publishes `startup/boot_completed`; use the command below only to
restart or replace it manually.

Recommended `lua_run_script_async` input:

```json
{
  "path": "/fatfs/skills/ble_hid_input/scripts/listen_ble_hid_input.lua",
  "args": {
    "gpio": 0,
    "active_level": 0,
    "input_id": "main"
  },
  "name": "ble_hid_input",
  "exclusive": "ble_hid_input",
  "replace": true,
  "timeout_ms": 0
}
```
