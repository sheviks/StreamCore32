#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    int sda_gpio;
    int scl_gpio;
    uint32_t freq_hz;
} i2c_bus_t;

typedef struct {
    i2c_bus_t *bus;
    i2c_master_dev_handle_t dev;
    uint8_t addr_7bit;
    uint32_t timeout_ms;
} i2c_device_t;

esp_err_t i2c_bus_init(i2c_bus_t *out, int sda_gpio, int scl_gpio, uint32_t freq_hz);
esp_err_t i2c_bus_deinit(i2c_bus_t *bus);

esp_err_t i2c_device_init(i2c_device_t *out, i2c_bus_t *bus, uint8_t addr_7bit, uint32_t timeout_ms);
esp_err_t i2c_device_deinit(i2c_device_t *dev);

// Basic register helpers (common for many I2C peripherals)
esp_err_t i2c_reg_read_u8 (i2c_device_t *dev, uint8_t reg, uint8_t *out, size_t count);
esp_err_t i2c_reg_write_u8(i2c_device_t *dev, uint8_t reg, uint8_t val);

esp_err_t i2c_reg_read_u16_le (i2c_device_t *dev, uint8_t reg, uint16_t *out); // little-endian
esp_err_t i2c_reg_write_u16_le(i2c_device_t *dev, uint8_t reg, uint16_t val);

#ifdef __cplusplus
}
#endif
