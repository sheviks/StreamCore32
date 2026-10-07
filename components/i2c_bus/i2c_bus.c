#include "i2c_bus.h"
#include "esp_log.h"

#include "esp_err.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>

#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "i2c_bus";

esp_err_t i2c_bus_init(i2c_bus_t *out, int sda_gpio, int scl_gpio, uint32_t freq_hz)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (i2c_bus_t){0};

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 1,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 0, // you have external pullups
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &out->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    out->sda_gpio = sda_gpio;
    out->scl_gpio = scl_gpio;
    out->freq_hz = freq_hz;
    return ESP_OK;
}

esp_err_t i2c_bus_deinit(i2c_bus_t *bus)
{
    if (!bus || !bus->bus) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_del_master_bus(bus->bus);
    bus->bus = NULL;
    return err;
}

esp_err_t i2c_device_init(i2c_device_t *out, i2c_bus_t *bus, uint8_t addr_7bit, uint32_t timeout_ms)
{
    if (!out || !bus || !bus->bus) return ESP_ERR_INVALID_ARG;
    *out = (i2c_device_t){0};

    i2c_device_config_t dev_cfg = {
        .device_address = addr_7bit,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = bus->freq_hz,
    };

    esp_err_t err = i2c_master_bus_add_device(bus->bus, &dev_cfg, &out->dev);
    if (err != ESP_OK) return err;

    out->bus = bus;
    out->addr_7bit = addr_7bit;
    out->timeout_ms = timeout_ms;
    return ESP_OK;
}

esp_err_t i2c_device_deinit(i2c_device_t *dev)
{
    if (!dev || !dev->dev) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_bus_rm_device(dev->dev);
    dev->dev = NULL;
    dev->bus = NULL;
    return err;
}

esp_err_t i2c_reg_read_u8(i2c_device_t *dev, uint8_t reg, uint8_t *out, size_t count)
{
    if (!dev || !dev->dev || !out) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(dev->dev, &reg, 1, out, count, dev->timeout_ms);
}

esp_err_t i2c_reg_write_u8(i2c_device_t *dev, uint8_t reg, uint8_t val)
{
    if (!dev || !dev->dev) return ESP_ERR_INVALID_ARG;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev->dev, buf, sizeof(buf), dev->timeout_ms);
}

esp_err_t i2c_reg_read_u16_le(i2c_device_t *dev, uint8_t reg, uint16_t *out)
{
    if (!dev || !dev->dev || !out) return ESP_ERR_INVALID_ARG;
    uint8_t raw[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(dev->dev, &reg, 1, raw, 2, dev->timeout_ms);
    if (err != ESP_OK) return err;
    *out = (uint16_t)(raw[0] | (raw[1] << 8));
    return ESP_OK;
}

esp_err_t i2c_reg_write_u16_le(i2c_device_t *dev, uint8_t reg, uint16_t val)
{
    if (!dev || !dev->dev) return ESP_ERR_INVALID_ARG;
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return i2c_master_transmit(dev->dev, buf, sizeof(buf), dev->timeout_ms);
}
