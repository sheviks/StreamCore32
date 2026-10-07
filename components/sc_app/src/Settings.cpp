#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "Logger.h"

#ifndef CONFIG_SC32_DEVICE_NAME
#define CONFIG_SC32_DEVICE_NAME "StreamCore32"
#endif
#ifndef CONFIG_SC32_LED_MODE
#define CONFIG_SC32_LED_MODE 3
#endif
#ifndef CONFIG_SC32_LED_BRIGHTNESS
#define CONFIG_SC32_LED_BRIGHTNESS 20
#endif
#ifndef CONFIG_QOBUZ_AUDIO_FORMAT
#define CONFIG_QOBUZ_AUDIO_FORMAT 7
#endif
#ifndef CONFIG_SPOTIFY_AUDIO_FORMAT
#define CONFIG_SPOTIFY_AUDIO_FORMAT 1
#endif

namespace sc32 {

namespace {
constexpr const char* kNs = "sc32cfg";
constexpr uint32_t kSaveDelayMs = 1500;

std::atomic<bool> g_flushReq{false};
SemaphoreHandle_t g_flushDone = nullptr;

// clamp helpers
template <class T>
T clampT(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

uint8_t getU8(nvs_handle_t h, const char* k, uint8_t def) {
  uint8_t v;
  return nvs_get_u8(h, k, &v) == ESP_OK ? v : def;
}
int8_t getI8(nvs_handle_t h, const char* k, int8_t def) {
  int8_t v;
  return nvs_get_i8(h, k, &v) == ESP_OK ? v : def;
}
uint16_t getU16(nvs_handle_t h, const char* k, uint16_t def) {
  uint16_t v;
  return nvs_get_u16(h, k, &v) == ESP_OK ? v : def;
}
uint32_t getU32(nvs_handle_t h, const char* k, uint32_t def) {
  uint32_t v;
  return nvs_get_u32(h, k, &v) == ESP_OK ? v : def;
}
uint32_t packLed(const LedColor& c) {
  return (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.fx;
}
LedColor unpackLed(uint32_t v) {
  LedColor c;
  c.r = v >> 24;
  c.g = v >> 16;
  c.b = v >> 8;
  c.fx = v & 0xff;
  return c;
}
std::string ledKey(size_t i) {
  return std::string("led_") + Settings::ledStates()[i].key;  // <= 15 chars
}
std::string toHex(const LedColor& c) {
  char b[8];
  snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
  return b;
}
bool fromHex(const std::string& s, LedColor& c) {
  const char* p = s.c_str();
  if (*p == '#')
    p++;
  if (strlen(p) != 6)
    return false;
  char* e = nullptr;
  unsigned long v = strtoul(p, &e, 16);
  if (!e || *e)
    return false;
  c.r = v >> 16;
  c.g = v >> 8;
  c.b = v;
  return true;
}
std::string getStr(nvs_handle_t h, const char* k) {
  size_t len = 0;
  if (nvs_get_str(h, k, nullptr, &len) != ESP_OK || len == 0 || len > 128)
    return "";
  std::string s(len, '\0');
  if (nvs_get_str(h, k, s.data(), &len) != ESP_OK)
    return "";
  s.resize(strnlen(s.c_str(), len));
  return s;
}

void sanitize(Settings::Values& v) {
  if (v.deviceName.size() > 48)
    v.deviceName.resize(48);
  v.spotifyFormat = clampT<uint8_t>(v.spotifyFormat, 0, 2);
  if (v.qobuzFormat != 5 && v.qobuzFormat != 6 && v.qobuzFormat != 7 &&
      v.qobuzFormat != 27)
    v.qobuzFormat = 7;
  v.radioResumeS = clampT<uint16_t>(v.radioResumeS, 0, 3600);
  v.volume = clampT<uint8_t>(v.volume, 0, 100);
  v.bassDb = clampT<uint8_t>(v.bassDb, 0, 15);
  v.bassFreqHz = clampT<uint8_t>(uint8_t(v.bassFreqHz / 10 * 10), 20, 150);
  v.trebleHalfDb = clampT<int8_t>(v.trebleHalfDb, -8, 7);
  v.trebleFreqKhz = clampT<uint8_t>(v.trebleFreqKhz, 1, 15);
  v.ledMode = clampT<uint8_t>(v.ledMode, 0, 3);
  v.ledBrightness = clampT<uint8_t>(v.ledBrightness, 1, 100);
  for (auto& c : v.led)
    c.fx = clampT<uint8_t>(c.fx, 0, 2);
}
}  // namespace

Settings& Settings::instance() {
  static Settings s;
  return s;
}

std::string Settings::defaultDeviceName() {
  return CONFIG_SC32_DEVICE_NAME;
}

void Settings::load() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (loaded_)
    return;
  Values v;
  v.spotifyFormat = CONFIG_SPOTIFY_AUDIO_FORMAT;
  v.qobuzFormat = CONFIG_QOBUZ_AUDIO_FORMAT;
  v.ledMode = CONFIG_SC32_LED_MODE;
  v.ledBrightness = CONFIG_SC32_LED_BRIGHTNESS;
  for (size_t i = 0; i < kLedStates; i++)
    v.led[i] = ledStates()[i].def;
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
    v.deviceName = getStr(h, "dev_name");
    v.spotifyEnabled = getU8(h, "sp_en", 1);
    v.qobuzEnabled = getU8(h, "qb_en", 1);
    v.dlnaEnabled = getU8(h, "dl_en", 1);
    v.spotifyFormat = getU8(h, "sp_fmt", v.spotifyFormat);
    v.qobuzFormat = getU8(h, "qb_fmt", v.qobuzFormat);
    v.radioResumeS = getU16(h, "radio_res", v.radioResumeS);
    v.volume = getU8(h, "vol", v.volume);
    v.restoreVolume = getU8(h, "vol_rest", 1);
    v.bassDb = getU8(h, "bass_db", 0);
    v.bassFreqHz = getU8(h, "bass_hz", 20);
    v.trebleHalfDb = getI8(h, "treb_hdb", 0);
    v.trebleFreqKhz = getU8(h, "treb_khz", 1);
    v.darkMode = getU8(h, "dark", 0);
    v.spectrum = getU8(h, "spectrum", 0);
    v.ledMode = getU8(h, "led_mode", v.ledMode);
    v.ledBrightness = getU8(h, "led_bri", v.ledBrightness);
    for (size_t i = 0; i < kLedStates; i++)
      v.led[i] = unpackLed(getU32(h, ledKey(i).c_str(), packLed(v.led[i])));
    nvs_close(h);
  }
  sanitize(v);
  v_ = v;
  boot_ = v;
  loaded_ = true;
  g_flushDone = xSemaphoreCreateBinary();
  // the NVS writes run in runWriter() (the main task, see there)
  SC32_LOG(info, "settings: device name \"%s\"", deviceName().c_str());
}

Settings::Values Settings::get() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return v_;
}

void Settings::update(const std::function<void(Values&)>& fn) {
  Values before, now;
  std::vector<Listener> ls;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    before = v_;
    fn(v_);
    sanitize(v_);
    now = v_;
    ls = listeners_;
  }
  for (auto& l : ls)
    l(before, now);
  dirty_.store(true);
  if (writer_)
    xTaskNotifyGive((TaskHandle_t)writer_);
}

void Settings::addListener(Listener l) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  listeners_.push_back(std::move(l));
}

void Settings::flush() {
  if (!writer_)
    return;
  g_flushReq.store(true);
  xTaskNotifyGive((TaskHandle_t)writer_);
  xSemaphoreTake(g_flushDone, pdMS_TO_TICKS(3000));
}

void Settings::runWriter() {
  // runs in the calling task (app_main: internal stack, which NVS writes
  // need, and otherwise idle — saves a task and its internal RAM)
  writer_ = xTaskGetCurrentTaskHandle();
  xTaskNotifyGive((TaskHandle_t)writer_);  // save what changed during boot
  writerTask(this);
}

void Settings::runLater(std::function<void()> job) {
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    jobs_.push_back(std::move(job));
  }
  if (writer_)
    xTaskNotifyGive((TaskHandle_t)writer_);
}

void Settings::writerTask(void* arg) {
  auto* self = static_cast<Settings*>(arg);
  auto runJobs = [self]() {
    bool any = false;
    for (;;) {
      std::vector<std::function<void()>> jobs;
      {
        std::lock_guard<std::recursive_mutex> lk(self->mu_);
        jobs.swap(self->jobs_);
      }
      if (jobs.empty())
        return any;
      any = true;
      for (auto& j : jobs)
        j();
    }
  };
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // queued jobs run at once; settings are saved after a quiet period
    bool onlyJobs = runJobs() && !self->dirty_.exchange(false);
    if (onlyJobs && !g_flushReq.load())
      continue;
    while (!g_flushReq.load() &&
           ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kSaveDelayMs)) > 0) {
      runJobs();
    }
    self->dirty_.store(false);
    self->save();
    if (g_flushReq.exchange(false))
      xSemaphoreGive(g_flushDone);
  }
}

void Settings::save() {
  Values v = get();
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
    SC32_LOG(error, "settings: cannot open NVS");
    return;
  }
  nvs_set_str(h, "dev_name", v.deviceName.c_str());
  nvs_set_u8(h, "sp_en", v.spotifyEnabled);
  nvs_set_u8(h, "qb_en", v.qobuzEnabled);
  nvs_set_u8(h, "dl_en", v.dlnaEnabled);
  nvs_set_u8(h, "sp_fmt", v.spotifyFormat);
  nvs_set_u8(h, "qb_fmt", v.qobuzFormat);
  nvs_set_u16(h, "radio_res", v.radioResumeS);
  nvs_set_u8(h, "vol", v.volume);
  nvs_set_u8(h, "vol_rest", v.restoreVolume);
  nvs_set_u8(h, "bass_db", v.bassDb);
  nvs_set_u8(h, "bass_hz", v.bassFreqHz);
  nvs_set_i8(h, "treb_hdb", v.trebleHalfDb);
  nvs_set_u8(h, "treb_khz", v.trebleFreqKhz);
  nvs_set_u8(h, "dark", v.darkMode);
  nvs_set_u8(h, "spectrum", v.spectrum);
  nvs_set_u8(h, "led_mode", v.ledMode);
  nvs_set_u8(h, "led_bri", v.ledBrightness);
  for (size_t i = 0; i < kLedStates; i++)
    nvs_set_u32(h, ledKey(i).c_str(), packLed(v.led[i]));
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK)
    SC32_LOG(error, "settings: save failed: %s", esp_err_to_name(e));
}

std::string Settings::deviceName() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return v_.deviceName.empty() ? defaultDeviceName() : v_.deviceName;
}


bool Settings::restartRequired() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto name = [](const Values& v) {
    return v.deviceName.empty() ? defaultDeviceName() : v.deviceName;
  };
  return name(v_) != name(boot_) || v_.spotifyEnabled != boot_.spotifyEnabled ||
         v_.qobuzEnabled != boot_.qobuzEnabled || v_.dlnaEnabled != boot_.dlnaEnabled;
}

const std::vector<std::pair<int, const char*>>& Settings::spotifyFormats() {
  static const std::vector<std::pair<int, const char*>> l = {
      {0, "96 kbps"}, {1, "160 kbps"}, {2, "320 kbps"}};
  return l;
}
const std::vector<std::pair<int, const char*>>& Settings::qobuzFormats() {
  static const std::vector<std::pair<int, const char*>> l = {
      {5, "MP3 320"}, {6, "CD 16/44.1"}, {7, "Hi-Res 96k"}, {27, "Hi-Res 192k"}};
  return l;
}
const std::array<LedStateInfo, kLedStates>& Settings::ledStates() {
  // order = LedState
  static const std::array<LedStateInfo, kLedStates> s = {{
      {"wifi_start", "WiFi starting", {0, 0, 255, (uint8_t)LedEffect::Blink}},
      {"wifi_lost", "No WiFi", {255, 0, 0, (uint8_t)LedEffect::Solid}},
      {"idle", "WiFi connected (idle)", {255, 140, 0, (uint8_t)LedEffect::Solid}},
      {"spotify", "Spotify connected", {0, 255, 0, (uint8_t)LedEffect::Solid}},
      {"qobuz", "Qobuz connected", {0, 120, 255, (uint8_t)LedEffect::Solid}},
      {"radio", "Radio playing", {255, 0, 160, (uint8_t)LedEffect::Solid}},
      {"sd", "SD player playing", {255, 255, 255, (uint8_t)LedEffect::Solid}},
      {"dlna", "DLNA playing", {0, 255, 200, (uint8_t)LedEffect::Solid}},
  }};
  return s;
}
const std::vector<std::pair<int, const char*>>& Settings::ledModes() {
  static const std::vector<std::pair<int, const char*>> l = {
      {0, "Off (LED not used)"},
      {1, "Only while WiFi starts"},
      {2, "WiFi status only"},
      {3, "All states"}};
  return l;
}
const std::vector<std::pair<int, const char*>>& Settings::ledEffects() {
  static const std::vector<std::pair<int, const char*>> l = {
      {0, "Solid"}, {1, "Blink"}, {2, "Pulse"}};
  return l;
}
const char* Settings::labelOf(const std::vector<std::pair<int, const char*>>& l,
                              int v) {
  for (auto& e : l)
    if (e.first == v)
      return e.second;
  return "?";
}

nlohmann::json Settings::toJson() {
  Values v = get();
  nlohmann::json j;
  j["device_name"] = deviceName();
  j["device_name_default"] = defaultDeviceName();
  j["hostname"] = deviceName();  // mDNS: http://<device name>.local/
  j["spotify_enabled"] = v.spotifyEnabled;
  j["qobuz_enabled"] = v.qobuzEnabled;
  j["dlna_enabled"] = v.dlnaEnabled;
  j["spotify_format"] = v.spotifyFormat;
  j["qobuz_format"] = v.qobuzFormat;
  j["radio_resume_s"] = v.radioResumeS;
  j["volume"] = v.volume;
  j["restore_volume"] = v.restoreVolume;
  j["bass_db"] = v.bassDb;
  j["bass_hz"] = v.bassFreqHz;
  j["treble_db"] = v.trebleHalfDb * 1.5;
  j["treble_khz"] = v.trebleFreqKhz;
  j["dark_mode"] = v.darkMode;
  j["spectrum"] = v.spectrum;
  j["led_mode"] = v.ledMode;
  j["led_brightness"] = v.ledBrightness;
  nlohmann::json leds = nlohmann::json::array();
  for (size_t i = 0; i < kLedStates; i++)
    leds.push_back({{"key", ledStates()[i].key},
                    {"label", ledStates()[i].label},
                    {"color", toHex(v.led[i])},
                    {"effect", v.led[i].fx}});
  j["led"] = leds;
  auto opts = [](const std::vector<std::pair<int, const char*>>& l) {
    nlohmann::json a = nlohmann::json::array();
    for (auto& e : l)
      a.push_back({{"value", e.first}, {"label", e.second}});
    return a;
  };
  j["spotify_formats"] = opts(spotifyFormats());
  j["qobuz_formats"] = opts(qobuzFormats());
  j["led_modes"] = opts(ledModes());
  j["led_effects"] = opts(ledEffects());
  return j;
}

bool Settings::applyJson(const nlohmann::json& j, std::string* error) {
  if (!j.is_object()) {
    if (error)
      *error = "not an object";
    return false;
  }
  auto num = [&](const char* k, double& out) {
    if (!j.contains(k))
      return false;
    const auto& x = j[k];
    if (x.is_number()) {
      out = x.get<double>();
      return true;
    }
    if (x.is_boolean()) {
      out = x.get<bool>() ? 1 : 0;
      return true;
    }
    if (x.is_string()) {
      char* e = nullptr;
      out = strtod(x.get_ref<const std::string&>().c_str(), &e);
      return e && *e == 0;
    }
    return false;
  };
  update([&](Values& v) {
    double d;
    if (j.contains("device_name") && j["device_name"].is_string()) {
      std::string n = j["device_name"].get<std::string>();
      // trim
      while (!n.empty() && std::isspace((unsigned char)n.back()))
        n.pop_back();
      size_t i = 0;
      while (i < n.size() && std::isspace((unsigned char)n[i]))
        i++;
      n.erase(0, i);
      v.deviceName = n == defaultDeviceName() ? "" : n;
    }
    if (num("spotify_enabled", d)) v.spotifyEnabled = d != 0;
    if (num("qobuz_enabled", d)) v.qobuzEnabled = d != 0;
    if (num("dlna_enabled", d)) v.dlnaEnabled = d != 0;
    if (num("spotify_format", d)) v.spotifyFormat = (uint8_t)d;
    if (num("qobuz_format", d)) v.qobuzFormat = (uint8_t)d;
    if (num("radio_resume_s", d)) v.radioResumeS = (uint16_t)std::max(0.0, d);
    if (num("volume", d)) v.volume = (uint8_t)std::max(0.0, std::min(100.0, d));
    if (num("restore_volume", d)) v.restoreVolume = d != 0;
    if (num("bass_db", d)) v.bassDb = (uint8_t)std::max(0.0, d);
    if (num("bass_hz", d)) v.bassFreqHz = (uint8_t)std::max(0.0, std::min(150.0, d));
    if (num("treble_db", d)) v.trebleHalfDb = (int8_t)lround(d / 1.5);
    if (num("treble_khz", d)) v.trebleFreqKhz = (uint8_t)std::max(0.0, d);
    if (num("dark_mode", d)) v.darkMode = d != 0;
    if (num("spectrum", d)) v.spectrum = d != 0;
    if (num("led_mode", d)) v.ledMode = (uint8_t)std::max(0.0, d);
    if (num("led_brightness", d)) v.ledBrightness = (uint8_t)std::max(1.0, std::min(100.0, d));
    // {"led":{"spotify":{"color":"#00ff00","effect":1}}} or the toJson() array
    if (j.contains("led")) {
      auto one = [&](const std::string& key, const nlohmann::json& o) {
        if (!o.is_object())
          return;
        for (size_t i = 0; i < kLedStates; i++) {
          if (key != ledStates()[i].key)
            continue;
          if (o.contains("color") && o["color"].is_string())
            fromHex(o["color"].get<std::string>(), v.led[i]);
          if (o.contains("effect") && o["effect"].is_number())
            v.led[i].fx = (uint8_t)o["effect"].get<int>();
          if (o.value("reset", false))
            v.led[i] = ledStates()[i].def;
        }
      };
      const auto& L = j["led"];
      if (L.is_object())
        for (auto it = L.begin(); it != L.end(); ++it)
          one(it.key(), it.value());
      else if (L.is_array())
        for (auto& o : L)
          if (o.is_object())
            one(o.value("key", ""), o);
    }
  });
  return true;
}

}  // namespace sc32
