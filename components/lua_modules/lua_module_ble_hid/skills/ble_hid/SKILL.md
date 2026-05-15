---
{
  "name": "ble_hid",
  "description": "Operate ESP-Claw BLE HID: start HID advertising, send media/keyboard/mouse actions, and update local button bindings.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# BLE HID

Use this skill whenever the user asks to start BLE HID, make the device show up
as a keyboard/mouse/media-control device, send media controls, type keys/text,
move/click/scroll the mouse, or update local button bindings.

## Hard Rules

1. Do not run `test_ble.lua` for BLE HID. It starts the generic BLE test service,
   not HID.
2. Do not use `test/test_ble_hid.lua` as a runtime entry point. It is for
   developer verification only.
3. In HID context, phrases like "start advertising", "start Bluetooth",
   "start HID", and "make the computer recognize it as keyboard/mouse" mean:
   run `/fatfs/skills/ble_hid/scripts/start_ble_hid.lua` first.
4. `start_ble_hid.lua` is idempotent. If BLE HID is already initialized,
   advertising, or connected, it returns status instead of failing.
5. Do not start the generic `ble` module advertising while BLE HID is running.

## Start BLE HID

```json
{"path":"/fatfs/skills/ble_hid/scripts/start_ble_hid.lua","args":{"name":"esp-claw-hid"},"timeout_ms":10000}
```

## Send One Action

Media:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"type":"media","key":"play_pause"},"timeout_ms":5000}
```

Input event:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"input_event":"button:main:single_click"},"timeout_ms":5000}
```

Keyboard key:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_key","key":"SPACE"},"timeout_ms":5000}
```

Keyboard combo:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_combo","keys":["CTRL","C"]},"timeout_ms":5000}
```

Text:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"type":"keyboard_text","text":"hello"},"timeout_ms":5000}
```

Mouse:

```json
{"path":"/fatfs/skills/ble_hid/scripts/send_ble_hid_action.lua","args":{"type":"mouse_move","dx":30,"dy":0},"timeout_ms":5000}
```

## Update Local Binding

Bind main button single click to Play/Pause:

```json
{"path":"/fatfs/skills/ble_hid/scripts/update_ble_hid_binding.lua","args":{"op":"update","source":"button","input_id":"main","event":"single_click","type":"media","key":"play_pause"},"timeout_ms":5000}
```

Reset bindings:

```json
{"path":"/fatfs/skills/ble_hid/scripts/update_ble_hid_binding.lua","args":{"op":"reset"},"timeout_ms":5000}
```

BLE HID is a composite device. Media, keyboard, and mouse reports are all
available at the same time; the action `type` decides which report is sent.
