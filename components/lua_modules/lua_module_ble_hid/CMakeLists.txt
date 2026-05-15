if(CONFIG_APP_CLAW_LUA_MODULE_BLE_HID)
    if(NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_NIMBLE_ENABLED OR NOT CONFIG_BT_NIMBLE_HID_SERVICE)
        message(FATAL_ERROR
            "APP_CLAW_LUA_MODULE_BLE_HID requires CONFIG_BT_ENABLED, "
            "CONFIG_BT_NIMBLE_ENABLED, and CONFIG_BT_NIMBLE_HID_SERVICE."
        )
    endif()

    set(lua_module_ble_hid_srcs "src/lua_module_ble_hid.c")
endif()

idf_component_register(
    SRCS
        ${lua_module_ble_hid_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        bt
        esp_hid
        freertos
        georgik__lua
        nvs_flash
)
