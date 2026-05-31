# I2C Bus Helper - Implementation

#include "i2c_bus.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2c.h"

static const char *TAG = "i2c_bus";

#define MAX_I2C_PORTS I2C_NUM_MAX

typedef struct {
    bool installed;
    SemaphoreHandle_t mutex;
} i2c_port_state_t;

static i2c_port_state_t s_ports[MAX_I2C_PORTS] = {0};

esp_err_t i2c_bus_init(i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio, uint32_t clk_speed_hz)
{
    ESP_RETURN_ON_FALSE(port >= 0 && port < MAX_I2C_PORTS, ESP_ERR_INVALID_ARG, TAG, "invalid port");

    if (s_ports[port].installed) {
        return ESP_OK;
    }

    i2c_config_t conf = {0};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda_gpio;
    conf.scl_io_num = scl_gpio;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = clk_speed_hz;

    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Use 0 queue size; higher-level code can serialize with our mutex.
    err = i2c_driver_install(port, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_ports[port].mutex) {
        s_ports[port].mutex = xSemaphoreCreateMutex();
        if (!s_ports[port].mutex) {
            ESP_LOGE(TAG, "failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    s_ports[port].installed = true;
    return ESP_OK;
}

static esp_err_t i2c_take_mutex(i2c_port_t port, TickType_t timeout_ticks)
{
    if (port < 0 || port >= MAX_I2C_PORTS) return ESP_ERR_INVALID_ARG;
    if (!s_ports[port].mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ports[port].mutex, timeout_ticks) == pdTRUE) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

static void i2c_give_mutex(i2c_port_t port)
{
    if (port < 0 || port >= MAX_I2C_PORTS) return;
    if (!s_ports[port].mutex) return;
    xSemaphoreGive(s_ports[port].mutex);
}

esp_err_t i2c_bus_write_bytes(i2c_port_t port, uint8_t addr, const uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(s_ports[port].installed, ESP_ERR_INVALID_STATE, TAG, "port not initialized");
    esp_err_t r = i2c_take_mutex(port, timeout_ticks);
    if (r != ESP_OK) return r;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (len) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(port, cmd, timeout_ticks);
    i2c_cmd_link_delete(cmd);

    i2c_give_mutex(port);
    return r;
}

esp_err_t i2c_bus_read_bytes(i2c_port_t port, uint8_t addr, uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(s_ports[port].installed, ESP_ERR_INVALID_STATE, TAG, "port not initialized");
    esp_err_t r = i2c_take_mutex(port, timeout_ticks);
    if (r != ESP_OK) return r;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);

    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    if (len) {
        i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    }

    i2c_master_stop(cmd);
    r = i2c_master_cmd_begin(port, cmd, timeout_ticks);
    i2c_cmd_link_delete(cmd);

    i2c_give_mutex(port);
    return r;
}
