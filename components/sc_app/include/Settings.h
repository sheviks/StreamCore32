#pragma once
// ============================================================================
//  Settings — persistent device settings (NVS namespace "sc32cfg").
//
//  • one place for everything the web UI and the e-paper UI can change
//  • defaults come from sdkconfig (e.g. CONFIG_SC32_DEVICE_NAME)
//  • update() changes the values in RAM at once, notifies the listeners and
//    schedules the NVS write (debounced, done by a small task with an
//    internal stack — callers may run on a PSRAM stack where flash writes
//    are not allowed, and dragging a slider must not wear out the flash)
//  • some settings only take effect after a restart (device name, enabling
//    or disabling a streaming service): restartRequired() tells the UIs
//
//  Thread safe.
// ============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace sc32 {

// ---- status LED ----------------------------------------------------------------
enum class LedState : uint8_t {
  WifiStart = 0,  // WiFi starting / connecting
  WifiLost,       // no WiFi
  Idle,           // WiFi connected, no source active
  Spotify,        // Spotify Connect connected
  Qobuz,          // Qobuz Connect connected
  Radio,          // radio playing
  SD,             // SD player playing
  Dlna,           // DLNA renderer playing
  Count
};
enum class LedEffect : uint8_t { Solid = 0, Blink, Pulse };
enum class LedMode : uint8_t { Off = 0, Startup, Wifi, All };
struct LedColor {
  uint8_t r = 0, g = 0, b = 0;  // 0,0,0 = LED off in this state
  uint8_t fx = 0;               // LedEffect
  bool operator==(const LedColor& o) const {
    return r == o.r && g == o.g && b == o.b && fx == o.fx;
  }
  bool operator!=(const LedColor& o) const { return !(*this == o); }
};
struct LedStateInfo {
  const char* key;    // JSON / NVS ("led_" + key)
  const char* label;  // UI
  LedColor def;
};
constexpr size_t kLedStates = (size_t)LedState::Count;

class Settings {
 public:
  struct Values {
    // ---- device ----
    std::string deviceName;  // shown in Spotify / Qobuz / mDNS
    // ---- streaming services ----
    bool spotifyEnabled = true;
    bool qobuzEnabled = true;
    bool dlnaEnabled = true;   // DLNA / UPnP renderer
    uint8_t spotifyFormat = 1;   // 0 = 96, 1 = 160, 2 = 320 kbps (Ogg Vorbis)
    uint8_t qobuzFormat = 7;     // 5 = MP3, 6 = CD, 7 = Hi-Res 96, 27 = 192
    uint16_t radioResumeS = 10;  // live radio: reconnect after a longer pause
    // ---- audio ----
    uint8_t volume = 50;  // 0..100
    bool restoreVolume = true;
    uint8_t bassDb = 0;        // 0..15
    uint8_t bassFreqHz = 20;   // 20..150 (10 Hz steps)
    int8_t trebleHalfDb = 0;   // -8..7 (x 1.5 dB)
    uint8_t trebleFreqKhz = 1; // 1..15
    // ---- display ----
    bool darkMode = false;
    // ---- spectrum (UDP broadcast of the VS1053 analyzer, port 6969) ----
    bool spectrum = false;  // off: its register reads interrupt the decoder feed
    // ---- status LED ----
    uint8_t ledMode = 3;         // LedMode
    uint8_t ledBrightness = 20;  // 1..100 %
    std::array<LedColor, kLedStates> led{};
  };

  using Listener = std::function<void(const Values& before, const Values& now)>;

  static Settings& instance();

  /** Read from NVS (call once after nvs_flash_init) and start the writer. */
  void load();

  Values get();
  /** Modify, notify listeners, save (debounced). */
  void update(const std::function<void(Values&)>& fn);
  /** Saves the changes; never returns.  Call at the end of app_main (the
   *  task must have an internal stack). */
  void runWriter();
  /** Run `job` on the settings task (internal stack): for flash / NVS work
   *  requested from tasks with a PSRAM stack (e.g. the web server). */
  void runLater(std::function<void()> job);
  /** Write pending changes now (e.g. before a restart). */
  void flush();

  void addListener(Listener l);

  /** Device name (falls back to the sdkconfig default). */
  std::string deviceName();
    static std::string defaultDeviceName();

  /** A setting changed that is only applied after a restart. */
  bool restartRequired();

  // ---- JSON for the web UI ---------------------------------------------------
  nlohmann::json toJson();
  /** Apply {"key": value, ...}; unknown keys are ignored. */
  bool applyJson(const nlohmann::json& j, std::string* error = nullptr);

  // option lists (value, label) for the UIs
  static const std::vector<std::pair<int, const char*>>& spotifyFormats();
  static const std::vector<std::pair<int, const char*>>& qobuzFormats();
  static const std::array<LedStateInfo, kLedStates>& ledStates();
  static const std::vector<std::pair<int, const char*>>& ledModes();
  static const std::vector<std::pair<int, const char*>>& ledEffects();
  static const char* labelOf(const std::vector<std::pair<int, const char*>>& l,
                             int v);

 private:
  Settings() = default;
  void save();
  static void writerTask(void* arg);

  std::recursive_mutex mu_;
  Values v_;
  Values boot_;  // values the firmware was started with
  bool loaded_ = false;
  std::vector<Listener> listeners_;
  std::vector<std::function<void()>> jobs_;  // guarded by mu_
  std::atomic<bool> dirty_{true};  // unsaved changes
  void* writer_ = nullptr;  // TaskHandle_t
};

}  // namespace sc32
