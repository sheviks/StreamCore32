#include "BQ27220.h"
#include "Logger.h"

#include "sdkconfig.h"
#ifdef CONFIG_BQ27220_LOG_VALUES
static constexpr bool REPORT_BAT_VALUES = true;
#else
static constexpr bool REPORT_BAT_VALUES = false;
#endif

static constexpr uint8_t REG_TEMP_0P1K   = 0x06; // 0x06-0x07 :contentReference[oaicite:2]{index=2}
static constexpr uint8_t REG_VOLT_MV     = 0x08; // 0x08-0x09 :contentReference[oaicite:3]{index=3}
static constexpr uint8_t REG_AVG_CURR_MA = 0x14; // 0x14-0x15 :contentReference[oaicite:4]{index=4}
static constexpr uint8_t REG_SOC_PCT     = 0x2C; // 0x2C-0x2D :contentReference[oaicite:5]{index=5}

BQ27220::BQ27220(i2c_bus_t *bus, uint8_t addr_7bit)
    : bell::Task("BQ27220", 4096, 0, 1,0) {
  if(!bus) return;
  i2c_device_init(&i2cDev_, bus, addr_7bit, 1000);
  addr_7bit_ = addr_7bit;
  mutex_ = xSemaphoreCreateMutex();
}
BQ27220::~BQ27220() {
  if(!i2cDev_.dev) return;
  i2c_device_deinit(&i2cDev_);
}

esp_err_t BQ27220::read_voltage_mv(uint16_t *mv){
  if (!mv) return ESP_ERR_INVALID_ARG;
  uint16_t raw = 0;
  esp_err_t err = i2c_reg_read_u16_le(&i2cDev_, REG_VOLT_MV, &raw);
  if (err != ESP_OK) return err;
  *mv = raw; // already mV :contentReference[oaicite:6]{index=6}
  return ESP_OK;
}
esp_err_t BQ27220::read_current_ma(int16_t *ma){
  if (!ma) return ESP_ERR_INVALID_ARG;
  uint16_t raw = 0;
  esp_err_t err = i2c_reg_read_u16_le(&i2cDev_, REG_AVG_CURR_MA, &raw);
  if (err != ESP_OK) return err;
  *ma = static_cast<int16_t>(raw); // signed mA :contentReference[oaicite:7]{index=7}
  return ESP_OK;
}
esp_err_t BQ27220::read_temp_c_x10(int16_t *c_x10) {
  if (!c_x10) return ESP_ERR_INVALID_ARG;
  uint16_t raw = 0;
  esp_err_t err = i2c_reg_read_u16_le(&i2cDev_, REG_TEMP_0P1K, &raw);
  if (err != ESP_OK) return err;

  // raw is 0.1 Kelvin. Convert to 0.1°C: C = K - 273.15
  // 0.1C = 0.1K - 2731.5 -> subtract 2732 with rounding
  *c_x10 = static_cast<int16_t>(raw) - 2732;
  return ESP_OK;
}
esp_err_t BQ27220::read_soc_pct_x10(uint16_t *pct_x10){
  if (!pct_x10) return ESP_ERR_INVALID_ARG;
  uint16_t raw = 0;
  esp_err_t err = i2c_reg_read_u16_le(&i2cDev_, REG_SOC_PCT, &raw);
  if (err != ESP_OK) return err;

  // TI table says "%" (not 0.1%). Treat as integer percent. :contentReference[oaicite:8]{index=8}
  // Provide x10 format for your API:
  if (raw > 100) raw = 100;
  *pct_x10 = raw * 10;
  return ESP_OK;
}


void BQ27220::runTask()  {
  if(!i2cDev_.dev) {
    SC32_LOG(error, "BQ27220 I2C device not initialized");
    return;
  }
  while (true) {
    if(xSemaphoreTake(mutex_, portMAX_DELAY)){
      if (read_voltage_mv(&mv_) == ESP_OK &&
          read_current_ma(&ma_) == ESP_OK &&
          read_temp_c_x10(&tc_) == ESP_OK &&
          read_soc_pct_x10(&soc_) == ESP_OK) {
        if(REPORT_BAT_VALUES)
          SC32_LOG(info, "V=%umV I=%dmA T=%d.%dC SOC=%u.%u%%\n",
                  (unsigned)mv_, (int)ma_,
                  (int)(tc_/10), (int)abs(tc_%10),
                  (unsigned)(soc_/10), (unsigned)(soc_%10));
      } else {
        BELL_SLEEP_MS(100);
        SC32_LOG(error, "read failed");
      }
      xSemaphoreGive(mutex_);
    }
    BELL_SLEEP_MS(1000);
  }
}