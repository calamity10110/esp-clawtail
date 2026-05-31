# panel_spi.h

#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_device_handle_t handle;
    int dc_gpio;
    int reset_gpio;
    int bl_gpio;
} panel_spi_t;

/**
 * Initialize SPI device for panel communications.
 * - host: SPI host (SPI2_HOST / SPI3_HOST)
 * - miso/mosi/clk/cs: pin numbers (use -1 for unused MISO when not required)
 * - dc_gpio: data/command gpio
 * - reset_gpio: reset gpio
 * - bl_gpio: backlight gpio (optional, -1 if none)
 */
esp_err_t panel_spi_init(panel_spi_t *ctx, spi_host_device_t host, int mosi_gpio, int miso_gpio, int sclk_gpio, int cs_gpio, int dc_gpio, int reset_gpio, int bl_gpio, int clock_hz);

/**
 * Deinitialize the SPI panel device.
 */
esp_err_t panel_spi_deinit(panel_spi_t *ctx);

/**
 * Send a command (single or multi-byte) to the panel. Command buffer must remain valid
 * until the function returns. For DMA transfers the buffer must be in DMA-capable memory.
 */
esp_err_t panel_spi_send_command(panel_spi_t *ctx, const uint8_t *cmd, size_t cmd_len, TickType_t timeout_ticks);

/**
 * Send data bytes to the panel. Data buffer must remain valid until the function returns.
 */
esp_err_t panel_spi_send_data(panel_spi_t *ctx, const uint8_t *data, size_t data_len, TickType_t timeout_ticks);

/**
 * Perform a full SPI transaction (command + data). Both buffers must remain valid until return.
 */
esp_err_t panel_spi_cmd_data(panel_spi_t *ctx, const uint8_t *cmd, size_t cmd_len, const uint8_t *data, size_t data_len, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
