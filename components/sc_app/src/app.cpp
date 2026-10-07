#include <MDNSService.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <mbedtls/aes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include "BellHTTPServer.h"
#include "BellLogger.h"  // for setDefaultLogger, AbstractLogger
#include "BellTask.h"
#include "WrappedSemaphore.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#include "NvsCreds.h"
#include "ZeroConf.h"

#include "SecureKeyHelper.h"

#include <inttypes.h>
#include "BellUtils.h"
#include "Logger.h"
#include "esp_log.h"

#include "WebUi.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "AudioControl.h"
#include "CrashLog.h"
#include "Settings.h"
#include "StreamManager.h"
#include "TimeSync.h"
#include "device.h"
#include "sc_app.h"
#include "sc_ui_device.h"

#if CONFIG_SC32_SPOTIFY
#include <SpotifyContext.h>
#include "ZeroConfServer.h"
#endif
#if CONFIG_SC32_DLNA
#include "DlnaRenderer.h"
#include "esp_mac.h"
#endif
#if CONFIG_SC32_SDCARD
#include "SD_Master.h"
#include "SdFiles.h"
#endif
#if CONFIG_SC32_STATUS_LED
#include "SK6812.h"
#endif
#if CONFIG_SC32_TOUCH || CONFIG_SC32_BATTERY
#include "i2c_bus.h"
#endif
#if CONFIG_SC32_BATTERY
#include "BQ27220.h"
#endif
#if CONFIG_SC32_DISPLAY_EINK
#include "gdey027T91.h"
#endif
#if CONFIG_SC32_TOUCH
#include "FT6X36.h"
#endif

#define SC32_VERSION "1.3.0"
#define SC32_CK(x)                                          \
  do {                                                      \
    esp_err_t __e = (x);                                    \
    if (__e != ESP_OK) {                                    \
      SC32_LOG(error, #x " -> %s\n", esp_err_to_name(__e)); \
      abort();                                              \
    }                                                       \
  } while (0)

// ---------------------------------------------------------------- hardware --
// (only the parts enabled in menuconfig: StreamCore32 -> Hardware)
#if CONFIG_SC32_TOUCH || CONFIG_SC32_BATTERY
i2c_bus_t bus;
#endif
#if CONFIG_SC32_BATTERY
std::shared_ptr<BQ27220> bq27220;
#endif
#if CONFIG_SC32_STATUS_LED
std::shared_ptr<SK6812> sk6812;
#endif
#if CONFIG_SC32_SDCARD
std::shared_ptr<SD_Master> sd_master;
#endif
#if CONFIG_SC32_DISPLAY_EINK
EpdSpi io;
// frame buffer object (~17 KB) in PSRAM: internal RAM is needed by WiFi / TLS
EXT_RAM_BSS_ATTR Gdey027T91 display(io);
#endif
#if CONFIG_SC32_TOUCH
std::shared_ptr<FT6X36> ts;
#endif

// ----------------------------------------------------------------- streams --
std::shared_ptr<AudioControl> audioControl;
std::shared_ptr<AudioControl::FeedControl> feedControl;  // tone / spectrum
std::shared_ptr<StreamManager> streams;
std::shared_ptr<Store> radioStore;
#if CONFIG_SC32_DLNA
std::unique_ptr<dlna::Renderer> dlnaRenderer;
#endif
SemaphoreHandle_t wifiSemaphore = NULL;

// ----------------------------------------------------------------- logging --
std::function<bool(const std::string&)> WsSendJsonSCLogger =
    [](const std::string& s) -> bool {
  WebUI::wsSendLog(s);  // queued, dropped when the browser is too slow
  return true;
};
// log lines also go to the "Log" page of the e-paper UI (no-op without)
std::function<bool(const std::string&)> EINK_LOG_CALLBACK =
    [](const std::string& s) -> bool {
  scui_device::log(s);
  return true;
};

// ------------------------------------------------------------ playback UI --
// Every "playback" / "queue" message of the active stream ends up here:
// forward it to the web UI and let the e-paper UI refresh.
static void onStreamUiMessage(const std::string& msg) {
  WebUI::wsSendJsonStatus(msg);
  if (msg.find("\"type\":\"queue\"") != std::string::npos)
    scui_device::notifyQueue();
  else
    scui_device::notifyPlayback();
}

#if CONFIG_SC32_STATUS_LED
// ------------------------------------------------------------- status LED --
enum class WifiLed : uint8_t { Starting, Connected, Lost };
static std::atomic<WifiLed> g_wifiLed{WifiLed::Starting};

static sc32::LedState ledState() {
  switch (g_wifiLed.load()) {
    case WifiLed::Starting:
      return sc32::LedState::WifiStart;
    case WifiLed::Lost:
      return sc32::LedState::WifiLost;
    default:
      break;
  }
  if (!streams)
    return sc32::LedState::Idle;
  StreamBase* s = streams->active();
  bool playing = s && s->playbackState() != StreamBase::PlaybackState::Stopped;
  switch (streams->current()) {
    case StreamManager::Service::Spotify:  // connected = selected in the app
      return sc32::LedState::Spotify;
    case StreamManager::Service::Qobuz:
      return sc32::LedState::Qobuz;
    case StreamManager::Service::Radio:
      return playing ? sc32::LedState::Radio : sc32::LedState::Idle;
    case StreamManager::Service::SD:
      return playing ? sc32::LedState::SD : sc32::LedState::Idle;
    case StreamManager::Service::Dlna:
      return playing ? sc32::LedState::Dlna : sc32::LedState::Idle;
    default:
      return sc32::LedState::Idle;
  }
}

#endif

static void sendPlaybackState(mg_connection* conn = nullptr) {
  if (StreamBase* s = streams->active()) {
    WebUI::wsSendJson(s->nowPlayingJson().dump(), conn);
    return;
  }
  nlohmann::json j;
  j["type"] = "playback";
  j["volume"] = streams->volume();
  WebUI::wsSendJson(j.dump(), conn);
}

static void saveStations(const Record& r) {
  radioStore->save(r);
  device::notifyStationsChanged();
}

static void playRadioStation(const std::string& url, const std::string& name) {
  auto list = device::stations();
  // a station from the list: play it by index so next/previous work
  for (size_t i = 0; i < list.size(); i++)
    if (list[i].url == url) {
      streams->playStation(list, i);
      return;
    }
  streams->playRadioUrl(url, name, list);
}

// ---------------------------------------------------------------- SD (web) --
// Only paths below the mount point, no "..".
static bool isSdPath(const std::string& p) {
  static const std::string root = "/sdcard";
  if (p.compare(0, root.size(), root) != 0)
    return false;
  if (p.size() > root.size() && p[root.size()] != '/')
    return false;
  return p.find("/..") == std::string::npos;
}

// ---------------------------------------------------------------- settings --
// VS10xx bass / treble enhancer (SCI_BASS) from the settings
static void applyTone(const sc32::Settings::Values& v) {
  if (!feedControl)
    return;
  int ta = std::max(-8, std::min(7, (int)v.trebleHalfDb));
  int tf = std::max(1, std::min(15, (int)v.trebleFreqKhz));
  int ba = std::max(0, std::min(15, (int)v.bassDb));
  int bf = std::max(2, std::min(15, (int)v.bassFreqHz / 10));
  uint16_t reg = uint16_t(((ta & 0x0F) << 12) | (tf << 8) | (ba << 4) | bf);
  auto fc = feedControl;
  fc->audioSink->feed_command(
      [fc, reg](uint8_t) { fc->audioSink->write_register(SCI_BASS, reg); });
}

// React to changed settings (from the web UI or the display)
static void onSettingsChanged(const sc32::Settings::Values& a,
                              const sc32::Settings::Values& b) {
  if (a.volume != b.volume && streams && streams->volume() != b.volume)
    streams->setVolume(b.volume);
  if (a.bassDb != b.bassDb || a.bassFreqHz != b.bassFreqHz ||
      a.trebleHalfDb != b.trebleHalfDb || a.trebleFreqKhz != b.trebleFreqKhz)
    applyTone(b);
#if CONFIG_SC32_SPOTIFY
  if (a.spotifyFormat != b.spotifyFormat) {
    spotify::preferredAudioFormat().store(b.spotifyFormat);
    SC32_LOG(info, "Spotify quality: %s (from the next connection)",
             sc32::Settings::labelOf(sc32::Settings::spotifyFormats(), b.spotifyFormat));
  }
#endif
#if CONFIG_SC32_QOBUZ
  if (a.qobuzFormat != b.qobuzFormat && streams && streams->qobuz)
    streams->qobuz->setMaxFormat(b.qobuzFormat);
#endif
#if CONFIG_SC32_WEBSTREAM
  if (a.radioResumeS != b.radioResumeS && streams && streams->radio)
    streams->radio->setLiveResumeAfterMs(uint32_t(b.radioResumeS) * 1000);
#endif
  if (a.darkMode != b.darkMode)
    scui_device::setDarkMode(b.darkMode);
  scui_device::notifyPlayback();
}

// remember the volume after the user changed it (restored at boot)
static void rememberVolume() {
  uint8_t v = streams->volume();
  if (sc32::Settings::instance().get().volume != v)
    sc32::Settings::instance().update([v](sc32::Settings::Values& s) { s.volume = v; });
}

// {"type":"settings","values":{..},"restart_required":..,"wifi":{..},"info":[[k,v]..]}
static nlohmann::json settingsJson() {
  auto& st = sc32::Settings::instance();
  nlohmann::json j;
  j["type"] = "settings";
  j["values"] = st.toJson();
  j["values"]["volume"] = streams->volume();
  j["restart_required"] = st.restartRequired();
  j["version"] = SC32_VERSION;
  nlohmann::json w;
  std::string ssid;
  w["connected"] = wifi_manager_is_connected();
  if (wifi_manager_get_connected_ssid(ssid) == ESP_OK)
    w["ssid"] = ssid;
  j["wifi"] = w;
  j["info"] = nlohmann::json::array();
  for (auto& kv : device::infoRows())
    j["info"].push_back({kv.first, kv.second});
  // what this firmware was built with (the web UI hides the rest)
  j["features"] = {
      {"spotify", sc32::kHasSpotify},
      {"qobuz", sc32::kHasQobuz},
      {"radio", sc32::kHasRadio},
      {"dlna", sc32::kHasDlna},
      {"sd", sc32::kHasSdCard},
      {"sd_player", sc32::kHasSdPlayer},
      {"display", sc32::kHasDisplay},
      {"led", sc32::kHasStatusLed},
  };
  return j;
}

static void restartDevice() {
  SC32_LOG(info, "restart requested");
  sc32::Settings::instance().flush();
  vTaskDelay(pdMS_TO_TICKS(300));  // let the web socket answer go out
  esp_restart();
}

// ------------------------------------------------------------------ web UI --
static void readWebUIJson(struct mg_connection* conn, char* data, size_t len) {
  if (!len)
    return sendPlaybackState(conn);
  std::string msg(data, len);
  nlohmann::json j = nlohmann::json::parse(msg, nullptr, false);
  if (j.is_discarded()) {
    SC32_LOG(error, "JSON parse error");
    return;
  }
  // (fs.write carries whole files: log the beginning only)
  SC32_LOG(info, "WS JSON received: %.*s%s", (int)std::min<size_t>(len, 200), data,
           len > 200 ? " ..." : "");
  const std::string type = j.value("type", "");

  if (type == "cmd") {
    // all transport / mode / queue commands go through StreamBase:
    // play, pause, toggle, next, prev, seek, seek_relative, seek_percent,
    // set_volume, volume_up/down, shuffle, repeat, play_index, get_queue ...
    StreamBase* s = streams->active();
    bool ok = s && s->handleCommand(j);
    if (!ok && j.value("cmd", "") == "set_volume" && j.contains("value") &&
        j["value"].is_number())
      streams->setVolume((uint8_t)std::min(100, std::max(0, j["value"].get<int>())));
    const std::string c = j.value("cmd", "");
    if (c == "set_volume" || c == "volume_up" || c == "volume_down")
      rememberVolume();
    sendPlaybackState();
    scui_device::notifyPlayback();
    return;
  }
  if (type == "settings.get") {
    WebUI::wsSendJson(settingsJson().dump(), conn);
    return;
  }
  if (type == "settings.set") {
    // {"type":"settings.set","values":{"device_name":"Kitchen","qobuz_format":6}}
    if (j.contains("values"))
      sc32::Settings::instance().applyJson(j["values"]);
    WebUI::wsSendJson(settingsJson().dump(), conn);
    return;
  }
  if (type == "wifi.connect") {
    // {"type":"wifi.connect","ssid":"..","password":".."}
    const std::string ssid = j.value("ssid", "");
    std::string pass = j.value("password", "");
    if (!ssid.empty()) {
      // (NVS: runs on the settings task, the web server threads have
      //  PSRAM stacks)
      sc32::Settings::instance().runLater([ssid, pass]() mutable {
        std::string cur;
        if (pass.empty() && wifi_manager_get_connected_ssid(cur) == ESP_OK && cur == ssid)
          wifi_manager_get_connected_pass(pass);  // keep the password
        SC32_LOG(info, "WiFi: connecting to %s (web)", ssid.c_str());
        wifi_manager_connect(ssid, pass, true);
      });
    }
    return;
  }
  if (type == "system.restart") {
    restartDevice();
    return;
  }
  if (type == "display.refresh") {
    scui_device::requestFullRefresh();
    return;
  }
  if (type == "radio.cmd") {
    const std::string cmd = j.value("cmd", "");
    if (!j.contains("station") || !j["station"].is_object())
      return;
    const std::string name = j["station"].value("name", "");
    const std::string url = j["station"].value("url", "");
    if (cmd == "play_station") {
      playRadioStation(url, name);
    } else if (cmd == "save_station" || cmd == "remove_station") {
      // station list lives in NVS: on the settings task (internal stack)
      sc32::Settings::instance().runLater([cmd, name, url]() {
        if (cmd == "save_station") {
          Record r;
          radioStore->load("stations", &r);
          if (r.userkey.empty())
            r.userkey = "stations";
          bool found = false;
          for (auto& s : r.fields)
            if (s.name == name) {
              s = {name, url};
              found = true;
            }
          if (!found)
            r.fields.push_back({name, url});
          saveStations(r);
        } else if (cmd == "remove_station") {
          Record r;
          radioStore->load("stations", &r);
          for (auto it = r.fields.begin(); it != r.fields.end(); it++) {
            if (it->name == name) {
              r.fields.erase(it);
              saveStations(r);
              break;
            }
          }
        }
      });
    }
    return;
  }
  if (type == "sd.cmd") {
    // {"type":"sd.cmd","cmd":"play","path":"/sdcard/Music/a.flac"}
    // {"type":"sd.cmd","cmd":"play_folder","path":"/sdcard/Music"}
    const std::string cmd = j.value("cmd", "");
    std::string path = j.value("path", "");
    if (path.empty())
      path = "/sdcard";
    if (!isSdPath(path)) {
      SC32_LOG(error, "SD: rejected path %s", path.c_str());
      return;
    }
    if (cmd == "play")
      streams->playFile(path);
    else if (cmd == "play_playlist")
      streams->playPlaylist(path, j.value("index", 0));
    else if (cmd == "play_folder")
      streams->playFolder(path);
    scui_device::notifyPlayback();
    return;
  }
  if (type.rfind("fs.", 0) == 0 || type.rfind("pl.", 0) == 0) {
    // file manager / playlists (see SdFiles.h)
#if CONFIG_SC32_SDCARD
    nlohmann::json r = sdfiles::handle(j);
#else
    nlohmann::json r = {{"type", "fs.result"}, {"op", type}, {"ok", false},
                        {"message", "no SD card support in this firmware"}};
#endif
    WebUI::wsSendJson(r.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), conn);
    return;
  }
  if (type == "page") {
    if (j["page"] == "page-radio") {
      nlohmann::json out;
      out["type"] = "radio";
      out["cmd"] = "stations";
      out["stations"] = nlohmann::json::array();
      for (auto& s : device::stations())
        out["stations"].push_back({{"name", s.name}, {"url", s.url}});
      WebUI::wsSendJson(out.dump());
    } else if (j["page"] == "page-debug") {
      nlohmann::json out;
      out["type"] = "debug";
      wifi_ap_record_t info{};
      if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        out["rssi"] = info.rssi;
        out["heap"] = esp_get_free_heap_size() / 1024;
        UBaseType_t taskCount = uxTaskGetNumberOfTasks();
        std::vector<TaskStatus_t> tasks(taskCount);
        uint32_t totalRunTime;
        taskCount =
            uxTaskGetSystemState(tasks.data(), taskCount, &totalRunTime);
        out["tasks"] = nlohmann::json::array();
        for (UBaseType_t i = 0; i < taskCount; ++i) {
          out["tasks"].push_back({{"task", tasks[i].pcTaskName},
                                  {"state", tasks[i].eCurrentState},
                                  {"priority", tasks[i].uxCurrentPriority},
                                  {"stack", tasks[i].usStackHighWaterMark}});
        }
      }
      WebUI::wsSendJson(out.dump());
    } else if (j["page"] == "page-settings") {
      WebUI::wsSendJson(settingsJson().dump(), conn);
    } else if (j["page"] == "page-player") {
      sendPlaybackState(conn);
      if (StreamBase* s = streams->active())
        s->publishQueue();
    }
  }
}

// ------------------------------------------------- spectrum over UDP (VS10xx) --
uint32_t SLEEP_TIME_MS = 50;
uint16_t UDP_PORT = 6969;
void udp_task(void* pvParameters) {
  int sock;
  struct sockaddr_in dest_addr;

  dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(UDP_PORT);

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

  int broadcast = 1;
  BELL_LOG(info, "spectrum", "UDP task started, broadcasting to port %u",
           UDP_PORT);
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
  SemaphoreHandle_t spectrum_semaphore = xSemaphoreCreateBinary();
  feedControl->audioSink->feed_command([](uint8_t) {
    feedControl->audioSink->write_register(SCI_AIADDR, 0x0d00);
  });

  while (1) {
    if (!sc32::Settings::instance().get().spectrum || !streams->isPlaying()) {
      BELL_SLEEP_MS(1000);
      continue;
    }
    int64_t start_time = esp_timer_get_time();
    char spectrum_data[66];
    spectrum_data[0] = 's';
    feedControl->audioSink->feed_command(
        [&spectrum_data, spectrum_semaphore](uint8_t) {
          constexpr uint16_t SPEC_BASE = 0x1800;
          feedControl->audioSink->write_register(SCI_WRAMADDR, SPEC_BASE + 2);
          uint16_t bands = feedControl->audioSink->read_register(SCI_WRAM);
          if (bands != 14)
            bands = 0;  // no spectrum data available
          spectrum_data[1] = (char)bands;
          if (bands != 0)
            feedControl->audioSink->write_register(SCI_WRAMADDR, SPEC_BASE + 4);
          for (int i = 0; i < bands; i++) {
            uint16_t val = feedControl->audioSink->read_register(SCI_WRAM);
            spectrum_data[i + 2] = val & 0x3F;                  // current
            spectrum_data[i + bands + 2] = (val >> 6) & 0x3F;  // peak
          }
          xSemaphoreGive(spectrum_semaphore);
        });
    if (xSemaphoreTake(spectrum_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE)
      continue;  // stream ended before the command ran
    sendto(sock, spectrum_data, spectrum_data[1] * 2 + 2, 0,
           (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    int64_t elapsed_us = esp_timer_get_time() - start_time;
    int64_t sleep_us = (SLEEP_TIME_MS * 1000) - elapsed_us;
    if (sleep_us > 0)
      vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
  }

  close(sock);
  vTaskDelete(NULL);
}

// ------------------------------------------------------------------- main --
void sc32_app_main(void) {
  crashlog::earlyInit(SC32_VERSION);  // first: captures the console from here on
  bell::setDefaultLogger();
  init_nvs();
  auto& settings = sc32::Settings::instance();
  settings.load();
  const sc32::Settings::Values cfg = settings.get();
  const std::string deviceName = settings.deviceName();
#if CONFIG_SC32_SPOTIFY
  spotify::preferredAudioFormat().store(cfg.spotifyFormat);
#endif
  radioStore = std::make_shared<Store>("radio");
#if CONFIG_SC32_STATUS_LED
  sk6812 = std::make_shared<SK6812>();
  sk6812->start(ledState);
#endif

#if CONFIG_SC32_DISPLAY_EINK
  display.init(false);
  display.setRotation(0);
#endif

#if CONFIG_SC32_TOUCH || CONFIG_SC32_BATTERY
  // I2C: touch + battery gauge
  SC32_CK(i2c_bus_init(&bus, CONFIG_GPIO_SDA, CONFIG_GPIO_SCL, 400000));
  i2c_master_bus_reset(bus.bus);
#endif
#if CONFIG_SC32_TOUCH
  ts = std::make_shared<FT6X36>(CONFIG_GPIO_TOUCH_INT, &bus);
  ts->begin(20, display.width(), display.height());
#endif
#if CONFIG_SC32_BATTERY
  bq27220 = std::make_shared<BQ27220>(&bus, CONFIG_BQ27220_I2C_ADDR);
  bq27220->startTask();
#endif

#if CONFIG_SC32_SDCARD
  // SD card (SDMMC, mounted at /sdcard; hot plug: device housekeeping task)
  sd_master = std::make_shared<SD_Master>();
  sd_master->mount();
#endif

  // SPI bus of the VS10xx decoder
  spi_bus_config_t bus_cfg;
  memset(&bus_cfg, 0, sizeof(spi_bus_config_t));
  bus_cfg.sclk_io_num = CONFIG_GPIO_HSPI_CLK;
  bus_cfg.mosi_io_num = CONFIG_GPIO_HSPI_MOSI;
  bus_cfg.miso_io_num = CONFIG_GPIO_HSPI_MISO;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4096;
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  assert(ret == ESP_OK);
  SC32_LOG(info, "hspi config done");

  // audio + the streams that work offline (SD) or need no login (radio)
  audioControl = std::make_shared<AudioControl>();
  feedControl = std::make_shared<AudioControl::FeedControl>(audioControl);
  streams = std::make_shared<StreamManager>(audioControl);
  streams->onUiMessage = onStreamUiMessage;

#if CONFIG_SC32_SDFILE
  streams->sd = std::make_shared<SDFileStream>(audioControl, SD_Master::kMountPoint);
  streams->sd->onError([](const std::string& m) { SC32_LOG(error, "%s", m.c_str()); });
  streams->attach(streams->sd.get(), StreamManager::Service::SD);
#endif
#if CONFIG_SC32_WEBSTREAM
  streams->radio = std::make_shared<WebStream>(audioControl);
  streams->radio->onError([](auto m) { SC32_LOG(error, "%s", m.c_str()); });
  streams->radio->setLiveResumeAfterMs(uint32_t(cfg.radioResumeS) * 1000);
  streams->attach(streams->radio.get(), StreamManager::Service::Radio);
#endif

  // saved audio settings
  if (cfg.restoreVolume)
    streams->setVolume(cfg.volume);
  applyTone(cfg);
  settings.addListener(onSettingsChanged);

  // device services: station list, SD hot plug, crash log, statistics
  device::Deps dd;
  dd.streams = streams.get();
#if CONFIG_SC32_SDCARD
  dd.sd = sd_master.get();
#endif
#if CONFIG_SC32_BATTERY
  dd.battery = bq27220.get();
#endif
  dd.radioStore = radioStore.get();
  dd.version = SC32_VERSION;
  device::start(dd);

  // WiFi driver first: the UI (status bar, settings) queries its state
  wifiSemaphore = xSemaphoreCreateBinary();
  wifi_manager_init_sta();

#if CONFIG_SC32_DISPLAY_EINK
  // e-paper UI (works before WiFi is up: SD card playback, settings, ...)
  scui_device::Deps deps;
  deps.display = &display;
#if CONFIG_SC32_TOUCH
  deps.touch = ts.get();
#endif
  deps.streams = streams.get();
  scui_device::start(deps);
#endif

  // WiFi
  wifi_manager_set_callbacks(
      [](const char* ssid, esp_ip4_addr_t ip) {
        SC32_LOG(info, "CONNECTED to %s, IP=" IPSTR, ssid, IP2STR(&ip));
#if CONFIG_SC32_STATUS_LED
        g_wifiLed.store(WifiLed::Connected);
        sk6812->wifiConnected();
#endif
        xSemaphoreGive(wifiSemaphore);
        scui_device::notifyPlayback();
      },
      [](const char* ssid, int reason, bool switching, bool userDisc) {
        SC32_LOG(error, "DISCONNECTED from %s reason=%d switching=%d user=%d",
                 ssid ? ssid : "(none)", reason, (int)switching,
                 (int)userDisc);
#if CONFIG_SC32_STATUS_LED
        // switching networks on purpose: "starting" again, else "lost"
        g_wifiLed.store(switching ? WifiLed::Starting : WifiLed::Lost);
#endif
        scui_device::notifyPlayback();
      });
  if (wifi_manager_connect_from_nvs() != ESP_OK)
    wifi_manager_connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD, false);

  // Wait for connection
  xSemaphoreTake(wifiSemaphore, portMAX_DELAY);
  SC32_LOG(info, "Connected to AP, start network services");
  mdns_init();
  mdns_hostname_set(deviceName.c_str());  // -> http://<device name>.local/
  mdns_instance_name_set(deviceName.c_str());
  InitZeroconf(deviceName, 7864);
  bell::MDNSService::registerService(deviceName, "_http", "_tcp", "", 80,
                                     {
                                         {"Name", deviceName},
                                     });
  SC32_LOG(info, "announcing as \"%s\" (http://%s.local/)", deviceName.c_str(),
           deviceName.c_str());
  WebUI::WebUI_start(80, readWebUIJson);
#if CONFIG_SC32_SDCARD
  sdfiles::setSdMaster(sd_master.get());
  sdfiles::setBeforeChangeHook([](const std::string& p) { streams->releaseSdPath(p); });
  if (auto* http = WebUI::server())
    sdfiles::registerHttp(*http);
#endif
  timesync::init();
  timesync::set_timezone_ch();
  if (!timesync::wait_until_valid(8000))
    SC32_LOG(error, "System time not valid (services needing a time stamp may fail)");

#if CONFIG_SC32_DLNA
  // DLNA / UPnP media renderer (control points push URLs to the device)
  if (cfg.dlnaEnabled) {
    streams->dlna = std::make_shared<DlnaStream>(audioControl);
    streams->dlna->onError([](const std::string& m) { SC32_LOG(error, "%s", m.c_str()); });
    streams->dlna->onActivate([] {
      streams->activate(StreamManager::Service::Dlna);
      scui_device::notifyPlayback();
    });
    streams->attach(streams->dlna.get(), StreamManager::Service::Dlna);
    dlna::Renderer::Config dc;
    dc.friendlyName = deviceName;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char uuid[40], serial[16];
    // stable per device (UPnP: the UDN must not change)
    snprintf(uuid, sizeof uuid, "5c333200-0000-1000-8000-%02x%02x%02x%02x%02x%02x", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(serial, sizeof serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    dc.uuid = uuid;
    dc.serial = serial;
    dc.modelNumber = SC32_VERSION;
    dc.port = CONFIG_SC32_DLNA_PORT;
    dc.localIp = [] {
      std::string ip = device::ipAddress();
      return ip == "-" ? std::string() : ip;
    };
    dlnaRenderer = std::make_unique<dlna::Renderer>(streams->dlna->player(), dc);
    streams->dlna->setRenderer(dlnaRenderer.get());
    if (!dlnaRenderer->start())
      SC32_LOG(error, "DLNA renderer could not be started");
  } else {
    SC32_LOG(info, "DLNA renderer is disabled in the settings");
  }
#endif

#if CONFIG_SC32_QOBUZ
  // Qobuz Connect (can be switched off in the settings: saves RAM)
  if (cfg.qobuzEnabled) {
    QobuzStream::Config qcfg;
    qcfg.name = deviceName;
    qcfg.maxFormat = cfg.qobuzFormat;
    streams->qobuz = std::make_shared<QobuzStream>(
        audioControl, qcfg, std::make_unique<SecureStore>("qobuz"),
        [](bool connected) {
          if (connected)
            streams->activate(StreamManager::Service::Qobuz);
          else
            streams->deactivate(StreamManager::Service::Qobuz);
          scui_device::notifyPlayback();
        });
    streams->attach(streams->qobuz.get(), StreamManager::Service::Qobuz);
  } else {
    SC32_LOG(info, "Qobuz Connect is disabled in the settings");
  }
#endif

#if CONFIG_SC32_SPOTIFY
  // Spotify Connect
  if (cfg.spotifyEnabled) {
    streams->spotify = std::make_shared<SpotifyStream>(
        audioControl, std::make_unique<SecureStore>("spotify"),
        [](bool connected) {
          if (connected)
            streams->activate(StreamManager::Service::Spotify);
          else
            streams->deactivate(StreamManager::Service::Spotify);
          scui_device::notifyPlayback();
        },
        deviceName);
    streams->attach(streams->spotify.get(), StreamManager::Service::Spotify);
  } else {
    SC32_LOG(info, "Spotify Connect is disabled in the settings");
  }
#endif

  // spectrum sender: sockets + sink commands only, stack can live in PSRAM
  xTaskCreatePinnedToCoreWithCaps(udp_task, "udp_task", 4096, NULL, 1, NULL, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // the main task stays alive as the settings writer (NVS needs an
  // internal stack, and this one would otherwise sit idle)
  settings.runWriter();
}
