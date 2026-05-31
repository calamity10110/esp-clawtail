# i2c_bus.h

#pragma once

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_init(i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_speed_hz);
esp_err_t i2c_bus_write_bytes(i2c_port_t port, uint8_t addr, const uint8_t *data, size_t len, TickType_t timeout_ticks);
esp_err_t i2c_bus_read_bytes(i2c_port_t port, uint8_t addr, uint8_t *data, size_t len, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
