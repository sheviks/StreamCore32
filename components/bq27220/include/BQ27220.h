#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "i2c_bus.h"
#ifndef ESP_PLATFORM
#define ESP_PLATFORM
#endif
#include "BellTask.h"

#ifndef BQ27220_I2C_ADDR_7BIT
#define BQ27220_I2C_ADDR_7BIT 0x55
#endif

class BQ27220 : public bell::Task {
 public:
  BQ27220(i2c_bus_t *bus, uint8_t addr_7bit = BQ27220_I2C_ADDR_7BIT);
  ~BQ27220();

  esp_err_t read_voltage_mv(uint16_t *mv);
  esp_err_t read_current_ma(int16_t *ma);
  esp_err_t read_temp_c_x10(int16_t *c_x10);
  esp_err_t read_soc_pct_x10(uint16_t *pct_x10);
  uint16_t mv() {
    if(!xSemaphoreTake(mutex_, portMAX_DELAY)) return 0;
    uint16_t mv = mv_;
    xSemaphoreGive(mutex_);
    return mv;
  }
  int16_t ma() {
    if(!xSemaphoreTake(mutex_, portMAX_DELAY)) return 0;
    int16_t ma = ma_;
    xSemaphoreGive(mutex_);
    return ma;
  }
  int16_t tc() {
    if(!xSemaphoreTake(mutex_, portMAX_DELAY)) return 0;
    int16_t tc = tc_;
    xSemaphoreGive(mutex_);
    return tc;
  }
  uint16_t soc() {
    if(!xSemaphoreTake(mutex_, portMAX_DELAY)) return 0;
    uint16_t soc = soc_;
    xSemaphoreGive(mutex_);
    return soc;
  }

protected:
  void runTask() override;

 private:
  i2c_device_t i2cDev_;
  uint8_t addr_7bit_;
  uint16_t mv_;
  int16_t ma_;
  int16_t tc_;
  uint16_t soc_;
  SemaphoreHandle_t mutex_;
};

