#include "SK6812.h"

#include "Logger.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_GPIO_LED_DATA
#define CONFIG_GPIO_LED_DATA -1
#endif

namespace {
constexpr uint32_t kResHz = 10 * 1000 * 1000;  // 0.1 us ticks
constexpr uint32_t kTickMs = 40;               // effect step
constexpr uint32_t kBlinkMs = 500;             // on / off
constexpr uint32_t kPulseMs = 2000;            // one breath
}  // namespace

void SK6812::start(Provider provider) {
  provider_ = std::move(provider);
  // small task, stack in PSRAM (no flash access in here)
  xTaskCreatePinnedToCoreWithCaps(taskFn, "status_led", 4096, this, 1, nullptr, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void SK6812::taskFn(void* arg) {
  static_cast<SK6812*>(arg)->loop();
}

bool SK6812::ensureStrip() {
  if (strip_)
    return true;
  if (stripFailed_ || CONFIG_GPIO_LED_DATA < 0)
    return false;
  led_strip_config_t sc = {};
  sc.strip_gpio_num = CONFIG_GPIO_LED_DATA;
  sc.max_leds = 1;
  sc.led_model = LED_MODEL_SK6812;
  sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rc = {};
  rc.clk_src = RMT_CLK_SRC_DEFAULT;
  rc.resolution_hz = kResHz;
  rc.mem_block_symbols = 0;  // default, no DMA needed for one LED
  esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &strip_);
  if (e != ESP_OK) {
    SC32_LOG(error, "status LED: init failed: %s", esp_err_to_name(e));
    strip_ = nullptr;
    stripFailed_ = true;
    return false;
  }
  return true;
}

void SK6812::write(uint8_t r, uint8_t g, uint8_t b) {
  if (written_ && last_[0] == r && last_[1] == g && last_[2] == b)
    return;  // unchanged: no update
  if (!ensureStrip())
    return;
  if (r || g || b) {
    led_strip_set_pixel(strip_, 0, r, g, b);
    led_strip_refresh(strip_);
  } else {
    led_strip_clear(strip_);
  }
  last_[0] = r;
  last_[1] = g;
  last_[2] = b;
  written_ = true;
}

void SK6812::loop() {
  uint32_t t = 0;  // ms, effect phase
  sc32::Settings::Values cfg = sc32::Settings::instance().get();
  uint32_t cfgAge = 0;
  for (;;) {
    if ((cfgAge += kTickMs) >= 500) {  // settings changes apply within 0.5 s
      cfgAge = 0;
      cfg = sc32::Settings::instance().get();
    }
    auto mode = (sc32::LedMode)cfg.ledMode;
    bool active = mode != sc32::LedMode::Off &&
                  !(mode == sc32::LedMode::Startup && wifiSeen_.load());
    if (!active) {
      // switch off once (only if we ever lit it), then leave the LED alone
      if (written_ && (last_[0] || last_[1] || last_[2]))
        write(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      cfgAge = 500;
      continue;
    }
    sc32::LedState st = provider_ ? provider_() : sc32::LedState::WifiStart;
    if (mode != sc32::LedMode::All && st > sc32::LedState::Idle)
      st = sc32::LedState::Idle;  // sources only in mode "all"
    const sc32::LedColor& c = cfg.led[(size_t)st];
    uint32_t level = 255;  // effect
    switch ((sc32::LedEffect)c.fx) {
      case sc32::LedEffect::Blink:
        level = (t % (2 * kBlinkMs)) < kBlinkMs ? 255 : 0;
        break;
      case sc32::LedEffect::Pulse: {
        uint32_t p = t % kPulseMs;  // triangle, never fully dark
        uint32_t tri = p < kPulseMs / 2 ? p * 2 * 255 / kPulseMs
                                        : (kPulseMs - p) * 2 * 255 / kPulseMs;
        level = 20 + tri * 235 / 255;
        break;
      }
      default:
        break;
    }
    auto scale = [&](uint8_t v) {
      return (uint8_t)((uint32_t)v * cfg.ledBrightness * level / (100 * 255));
    };
    write(scale(c.r), scale(c.g), scale(c.b));
    bool animated = c.fx != 0 && (c.r || c.g || c.b);
    vTaskDelay(pdMS_TO_TICKS(animated ? kTickMs : 120));
    t += animated ? kTickMs : 120;
  }
}
