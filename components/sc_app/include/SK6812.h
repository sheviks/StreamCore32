#pragma once
// ============================================================================
//  SK6812 — status LED (one addressable RGB LED on CONFIG_GPIO_LED_DATA).
//
//  The colour follows the device state (see sc32::LedState): a provider
//  callback tells the current state, the settings tell colour, effect
//  (solid / blink / pulse), brightness and mode:
//    Off      the LED is never written (the RMT driver is not even created)
//    Startup  only while WiFi starts, dark once WiFi is connected
//    Wifi     WiFi states only (starting / lost / connected)
//    All      WiFi states + streaming sources
//  A state with colour 0,0,0 switches the LED off.  The LED is only written
//  when its colour changes (no periodic refresh for solid colours).
// ============================================================================
#include <atomic>
#include <cstdint>
#include <functional>

#include "Settings.h"
#include "led_strip.h"

class SK6812 {
 public:
  using Provider = std::function<sc32::LedState()>;

  SK6812() = default;
  /** Start the LED task (state from `provider`). */
  void start(Provider provider);
  /** WiFi got connected (ends the "startup" mode). */
  void wifiConnected() { wifiSeen_.store(true); }

 private:
  static void taskFn(void* arg);
  void loop();
  bool ensureStrip();
  void write(uint8_t r, uint8_t g, uint8_t b);

  Provider provider_;
  led_strip_handle_t strip_ = nullptr;
  bool stripFailed_ = false;
  bool written_ = false;
  uint8_t last_[3] = {0, 0, 0};
  std::atomic<bool> wifiSeen_{false};
};
