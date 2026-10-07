#include "sc_ui_device.h"
#if CONFIG_SC32_DISPLAY_EINK
#include <algorithm>

#include <dirent.h>
#include <sys/stat.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <atomic>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "Logger.h"
#include "Settings.h"
#include "StreamManager.h"
#include "device.h"
#include "sc_app.h"
#include "gdey027T91.h"
#include "wifi_manager.h"
#include "FT6X36.h"  // touch frame types (also without a touch panel)
#if CONFIG_SC32_SDCARD
#include "SD_Master.h"
#endif

#include "sc_ui_app.h"  // einkui + the app (must come after the drivers)

using scui_device::Deps;

namespace {

Deps g;
einkui::UI* g_ui = nullptr;
einkui::DrawCtx g_ctx;
scui::App* g_app = nullptr;
SemaphoreHandle_t g_mu = nullptr;  // serialises all UI work
TaskHandle_t g_task = nullptr;

std::mutex g_logMu;
std::deque<std::string> g_logs;
constexpr size_t kMaxQueuedLogs = 32;


struct UiLock {
  UiLock() { xSemaphoreTakeRecursive(g_mu, portMAX_DELAY); }
  ~UiLock() { xSemaphoreGiveRecursive(g_mu); }
};

uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
//  file system adapter for einkui::FilePage (FATFS through POSIX)
// ---------------------------------------------------------------------------
struct PosixFS : einkui::IFileSystem {
  bool list(const char* path, Entry* out, size_t& count, size_t max) override {
    count = 0;
    if (!device::sdInfo().mounted)
      return false;
    DIR* dir = opendir(path);
    if (!dir)
      return false;
    struct dirent* ent;
    while (count < max && (ent = readdir(dir)) != nullptr) {
      if (ent->d_name[0] == '.')
        continue;  // ".", "..", hidden / macOS "._" files
      const size_t nl = strlen(ent->d_name);
      if (nl >= sizeof(out[count].name))
        continue;  // name does not fit the list entry: it could not be opened
      memcpy(out[count].name, ent->d_name, nl + 1);
      std::string full = std::string(path) + "/" + ent->d_name;
      struct stat st;
      bool haveStat = stat(full.c_str(), &st) == 0;
      out[count].isDir = ent->d_type == DT_DIR ||
                         (ent->d_type == DT_UNKNOWN && haveStat && S_ISDIR(st.st_mode));
      out[count].size = (!out[count].isDir && haveStat) ? (uint32_t)st.st_size : 0;
      ++count;
    }
    closedir(dir);
    return true;
  }
} g_fs;

// ---------------------------------------------------------------------------
//  backend
// ---------------------------------------------------------------------------
class DeviceBackend : public scui::Backend {
 public:
  DeviceBackend() = default;

  // ---- active stream ----
  scui::NowPlaying nowPlaying() override {
    scui::NowPlaying out;
    out.volume = g.streams->volume();
    StreamBase* s = g.streams->active();
    if (!s)
      return out;
    StreamBase::NowPlaying np = s->nowPlaying();
    out.hasSource = true;
    out.state = scui::PlayState(int(np.state));
    out.track.id = np.track.id;
    out.track.title = np.track.title;
    out.track.artist = np.track.artist;
    out.track.album = np.track.album;
    out.track.durationMs = np.track.durationMs;
    out.positionMs = np.positionMs;
    out.source = np.source;
    out.quality = np.quality;
    out.volume = np.volume;
    out.shuffle = np.shuffle;
    out.repeat = scui::Repeat(int(np.repeat));
    out.caps.pause = np.caps.pause;
    out.caps.seek = np.caps.seek;
    out.caps.next = np.caps.next;
    out.caps.previous = np.caps.previous;
    out.caps.shuffle = np.caps.shuffle;
    out.caps.repeat = np.caps.repeat;
    out.caps.queue = np.caps.queue;
    out.caps.playQueueItem = np.caps.playQueueItem;
    out.caps.volume = np.caps.volume;
    return out;
  }
  void togglePause() override {
    StreamBase* s = g.streams->active();
    if (!s)
      return;
    if (s->playbackState() == StreamBase::PlaybackState::Stopped)
      s->resume();  // restarts SD / radio after the end
    else
      s->togglePause();
  }
  bool next() override {
    StreamBase* s = g.streams->active();
    return s && s->next();
  }
  bool previous() override {
    StreamBase* s = g.streams->active();
    return s && s->previous();
  }
  bool seek(uint32_t ms) override {
    StreamBase* s = g.streams->active();
    return s && s->seek(ms);
  }
  bool setShuffle(bool on) override {
    StreamBase* s = g.streams->active();
    return s && s->setShuffle(on);
  }
  bool setRepeat(scui::Repeat r) override {
    StreamBase* s = g.streams->active();
    return s && s->setRepeat(StreamBase::RepeatMode(int(r)));
  }
  void setVolume(uint8_t v) override {
    g.streams->setVolume(v);
    // remember it (restored at boot); the settings listener sees no change
    sc32::Settings::instance().update([v](sc32::Settings::Values& s) { s.volume = v; });
  }
  std::vector<scui::Track> queue(size_t maxItems) override {
    std::vector<scui::Track> out;
    StreamBase* s = g.streams->active();
    if (!s)
      return out;
    for (auto& t : s->getQueue(maxItems)) {
      scui::Track x;
      x.id = t.id;
      x.title = t.title;
      x.artist = t.artist;
      x.album = t.album;
      x.durationMs = t.durationMs;
      out.push_back(std::move(x));
    }
    return out;
  }
  bool playQueueItem(size_t i) override {
    StreamBase* s = g.streams->active();
    return s && s->playQueueItem(i);
  }
  void stopPlayback() override { g.streams->stopAll(); }

  // ---- radio ----
  std::vector<scui::Station> stations() override {
    std::vector<scui::Station> out;
    for (auto& s : device::stations())
      out.push_back({s.name, s.url});
    return out;
  }
  bool playStation(size_t i) override {
    return g.streams->playStation(device::stations(), i);
  }
  int currentStation() override {
#if CONFIG_SC32_WEBSTREAM
    if (g.streams->current() != StreamManager::Service::Radio ||
        !g.streams->radio)
      return -1;
    return g.streams->radio->currentStationIndex();
#else
    return -1;
#endif
  }

  // ---- SD ----
  einkui::IFileSystem* fileSystem() override { return &g_fs; }
  const char* sdRoot() override { return "/sdcard"; }
  scui::SdInfo sdInfo() override {
    scui::SdInfo o;
    device::SdInfo i = device::sdInfo();
    o.present = i.present;
    o.mounted = i.mounted;
    o.name = i.name;
    o.type = i.type;
    o.totalBytes = i.totalBytes;
    o.freeBytes = i.freeBytes;
    return o;
  }
  bool sdRemount() override { return device::sdRemount(); }
  bool playFile(const std::string& path) override {
    return g.streams->playFile(path);
  }
  bool playFolder(const std::string& dir) override {
    return g.streams->playFolder(dir);
  }

  // ---- WiFi ----
  bool wifiConnected() override { return device::wifiConnected(); }
  int wifiRssi() override { return device::wifiRssi(); }
  std::string wifiSsid() override { return device::wifiSsid(); }
  std::string ipAddress() override { return device::ipAddress(); }
  void wifiConnect(const std::string& ssid, const std::string& pass) override {
    std::string p = pass;
    if (p.empty() && ssid == wifiSsid())
      wifi_manager_get_connected_pass(p);  // "unchanged"
    SC32_LOG(info, "WiFi: connecting to %s", ssid.c_str());
    // wifi_manager_connect() saves to NVS (flash): that must not run on the
    // UI task (its stack is in PSRAM), so it gets a short-lived task with an
    // internal stack.
    auto* args = new std::pair<std::string, std::string>(ssid, p);
    if (xTaskCreate(
            [](void* a) {
              auto* kv = static_cast<std::pair<std::string, std::string>*>(a);
              wifi_manager_connect(kv->first, kv->second, true);
              delete kv;
              vTaskDelete(nullptr);
            },
            "wifi_conn", 4096, args, 3, nullptr) != pdPASS) {
      SC32_LOG(error, "WiFi: no memory for the connect task");
      delete args;
    }
  }

  // ---- audio ----
  // the settings are the source of truth; main.cpp writes the decoder
  // register when they change (applyTone)
  scui::Tone tone() override {
    auto v = sc32::Settings::instance().get();
    scui::Tone t;
    t.bassDb = v.bassDb;
    t.bassFreqHz = v.bassFreqHz;
    t.trebleDb = v.trebleHalfDb * 1.5f;
    t.trebleFreqKhz = v.trebleFreqKhz;
    return t;
  }
  void setTone(const scui::Tone& t) override {
    sc32::Settings::instance().update([&t](sc32::Settings::Values& v) {
      v.bassDb = (uint8_t)std::max(0, t.bassDb);
      v.bassFreqHz = (uint8_t)std::max(20, t.bassFreqHz);
      v.trebleHalfDb = (int8_t)lroundf(t.trebleDb / 1.5f);
      v.trebleFreqKhz = (uint8_t)std::max(1, t.trebleFreqKhz);
    });
  }

  // ---- settings ----
  int option(const std::string& k) override {
    auto v = sc32::Settings::instance().get();
    if (k == "spotify_format") return v.spotifyFormat;
    if (k == "qobuz_format") return v.qobuzFormat;
    if (k == "spotify_enabled") return v.spotifyEnabled;
    if (k == "qobuz_enabled") return v.qobuzEnabled;
    if (k == "dlna_enabled") return v.dlnaEnabled;
    if (k == "dark_mode") return v.darkMode;
    if (k == "restore_volume") return v.restoreVolume;
    if (k == "led_mode") return v.ledMode;
    if (k == "led_brightness") return v.ledBrightness;
    return 0;
  }
  bool has(const char* f) override {
    std::string k = f;
    if (k == "spotify") return sc32::kHasSpotify;
    if (k == "qobuz") return sc32::kHasQobuz;
    if (k == "radio") return sc32::kHasRadio;
    if (k == "dlna") return sc32::kHasDlna;
    if (k == "sd") return sc32::kHasSdCard;
    if (k == "led") return sc32::kHasStatusLed;
    return true;
  }
  void setOption(const std::string& k, int value) override {
    nlohmann::json j;
    j[k] = value;
    sc32::Settings::instance().applyJson(j);
  }
  std::string deviceName() override { return sc32::Settings::instance().deviceName(); }
  void setDeviceName(const std::string& n) override {
    nlohmann::json j;
    j["device_name"] = n;
    sc32::Settings::instance().applyJson(j);
  }
  bool restartRequired() override { return sc32::Settings::instance().restartRequired(); }
  void restart() override {
    sc32::Settings::instance().flush();
    esp_restart();
  }

  // ---- system ----
  scui::Battery battery() override {
    device::Battery d = device::battery();
    scui::Battery b;
    b.available = d.available;
    b.percent = d.percent;
    b.millivolts = d.millivolts;
    b.milliamps = d.milliamps;
    b.tempDeciC = d.tempDeciC;
    return b;
  }
  std::string clockText() override {
    time_t now = time(nullptr);
    if (now < 1700000000)  // not synced yet
      return "";
    struct tm tmv;
    localtime_r(&now, &tmv);
    char b[8];
    strftime(b, sizeof b, "%H:%M", &tmv);
    return b;
  }
  std::string version() override { return device::version(); }
  std::vector<std::pair<std::string, std::string>> systemInfo() override {
    return device::systemInfo();
  }

 private:
};

DeviceBackend* g_be = nullptr;
std::atomic<int> g_darkReq{-1};        // -1 = nothing, 0/1 = set dark mode
std::atomic<bool> g_redrawReq{false};  // full refresh requested

// ---------------------------------------------------------------------------
//  touch + task
// ---------------------------------------------------------------------------
// The FT6X36 task has a small stack: it only queues the frame, the UI task
// runs the handlers (which may call into Spotify / Qobuz / the SD player).
QueueHandle_t g_touchQ = nullptr;
void wake();

#if CONFIG_SC32_TOUCH
void onTouchFrame(const TTouchFrame& f) {
  if (!g_app || !g_touchQ)
    return;
  if (xQueueSend(g_touchQ, &f, 0) != pdTRUE) {
    TTouchFrame dropped;  // full: drop the oldest, keep the newest (lift-up)
    xQueueReceive(g_touchQ, &dropped, 0);
    xQueueSend(g_touchQ, &f, 0);
  }
  wake();
}
#endif

void uiTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    uint32_t now = nowMs();
    TTouchFrame tf;
    while (g_touchQ && xQueueReceive(g_touchQ, &tf, 0) == pdTRUE) {
      UiLock l;
      einkui::Parent* before = g_ui->activePage();
      if ((uint8_t)tf.p[0].event <= 1)  // press / release (not every move)
        SC32_LOG(debug, "touch %s x=%u y=%u n=%u",
                 (uint8_t)tf.p[0].event == 0 ? "down" : "up", tf.p[0].x,
                 tf.p[0].y, tf.touches);
      g_app->handleTouch(tf);
      if (g_ui->activePage() != before)
        SC32_LOG(info, "ui: page %s -> %s", before ? before->label().c_str() : "-",
                 g_ui->activePage()->label().c_str());
    }
    std::deque<std::string> logs;
    {
      std::lock_guard<std::mutex> lk(g_logMu);
      logs.swap(g_logs);
    }
    UiLock l;
    int dark = g_darkReq.exchange(-1);
    if (dark >= 0)
      g_app->setDarkMode(dark == 1);
    if (g_redrawReq.exchange(false))
      g_app->redrawAll();
    for (auto& s : logs)
      g_app->pushLog(s);
    g_app->tick(now);
  }
}

void wake() {
  if (g_task)
    xTaskNotifyGive(g_task);
}

}  // namespace

// ---------------------------------------------------------------------------
//  e-paper refresh policy used by einkui (see sc_ui_app.h)
// ---------------------------------------------------------------------------
namespace scui {
void epdFullUpdate(void* d) {
  // Whole-page redraws (page change, list scroll, new track) always get a
  // real full refresh.  A fast partial refresh of a mostly changed screen
  // leaves the old image in the panel; it creeps back within ~20 s and every
  // following partial refresh drives parts of it again, so the panel seems
  // to flip between the old and the new page.  Partial refreshes are only
  // used for single elements (buttons, sliders, progress, status bar).
  fullRefreshRequest().store(false);
  static_cast<Gdey027T91*>(d)->update();
}
void epdWindowUpdate(void* d, int16_t x, int16_t y, int16_t w, int16_t h) {
  if (w <= 0 || h <= 0)
    return;
  static_cast<Gdey027T91*>(d)->updateWindow(x, y, w, h);
}
}  // namespace scui

// ---------------------------------------------------------------------------
//  public API
// ---------------------------------------------------------------------------
namespace scui_device {

void setDarkMode(bool on) {
  g_darkReq.store(on ? 1 : 0);
  wake();
}

void requestFullRefresh() {
  scui::fullRefreshRequest().store(true);
  g_redrawReq.store(true);
  wake();
}

void notifyPlayback() {
  if (g_app) {
    g_app->notifyPlayback();
    wake();
  }
}

void notifyQueue() {
  if (g_app) {
    g_app->notifyQueue();
    wake();
  }
}

void log(const std::string& line) {
  std::lock_guard<std::mutex> lk(g_logMu);
  // "file.cpp:123 message" -> "message" (the screen is narrow)
  std::string s = line;
  size_t sp = s.find(' ');
  if (sp != std::string::npos && sp < 40 && s.find(':') < sp)
    s.erase(0, sp + 1);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  g_logs.push_back(std::move(s));
  while (g_logs.size() > kMaxQueuedLogs)
    g_logs.pop_front();
}

void start(const Deps& deps) {
  g = deps;
  g_mu = xSemaphoreCreateRecursiveMutex();
  g.display->setMonoMode(true);  // partial window refresh needs mono mode
  g_ctx = einkui::makeDrawCtx(*g.display);
  // The UI consists of a few hundred small objects (elements, strings,
  // callbacks) that live forever.  Below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
  // they would all land in internal RAM, which WiFi / TLS / the PHY need
  // (out of internal RAM -> esp_timer_create() fails in phy_track_pll_init).
  // Build the UI with PSRAM as the preferred heap for small blocks.
#if CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(16);
#endif
  g_ui = new einkui::UI();
  g_be = new DeviceBackend();
  g_app = new scui::App(*g_be, *g_ui, g_ctx);
  {
    UiLock l;
    g_app->build();
  }
#if CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
  {
    UiLock l;
    scui::fullRefreshRequest().store(true);  // first draw: clean panel
    g_app->start();
  }
  g_touchQ = xQueueCreate(16, sizeof(TTouchFrame));
  // Stack in PSRAM: the UI task draws, reads state and runs the touch
  // handlers, but never touches flash/NVS (the WiFi save runs in its own
  // task, the station list is cached).  Internal RAM is kept for WiFi/TLS.
  // Priority 1: below the VS1053 feeder (vs1053_task, prio 2, same core) —
  // drawing and e-paper transfers must never delay the audio data.
  if (xTaskCreatePinnedToCoreWithCaps(uiTask, "ui", 16 * 1024, nullptr, 1, &g_task,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    xTaskCreatePinnedToCore(uiTask, "ui", 8 * 1024, nullptr, 1, &g_task, 0);
#if CONFIG_SC32_TOUCH
  if (g.touch) {
    g.touch->setRotation(g.display->getRotation());
    g.touch->registerFrameHandler(onTouchFrame);
  }
#endif
}

}  // namespace scui_device

#endif  // CONFIG_SC32_DISPLAY_EINK
