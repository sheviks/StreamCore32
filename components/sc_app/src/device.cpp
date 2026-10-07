#include "device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

#include "CrashLog.h"
#include "Logger.h"
#include "NvsCreds.h"
#include "Settings.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sc_ui_device.h"
#include "sdkconfig.h"
#include "wifi_manager.h"
#if CONFIG_SC32_SDCARD
#include "SD_Master.h"
#endif
#if CONFIG_SC32_BATTERY
#include "BQ27220.h"
#endif

namespace device {
namespace {

Deps g;
std::mutex g_stMu;
std::vector<RadioStation> g_stations;
bool g_stDirty = true;

uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// CPU load per task since the last call (needs
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
void logCpu() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  static std::map<UBaseType_t, uint32_t> last;
  static uint32_t lastTotal = 0;
  UBaseType_t n = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> st(n + 4);
  uint32_t total = 0;
  n = uxTaskGetSystemState(st.data(), st.size(), &total);
  uint32_t dt = total - lastTotal;
  lastTotal = total;
  struct Row {
    std::string name;
    uint32_t pct10;
    UBaseType_t prio;
    BaseType_t core;
    uint32_t stack;
  };
  std::vector<Row> rows;
  std::map<UBaseType_t, uint32_t> now;
  for (UBaseType_t i = 0; i < n; i++) {
    uint32_t rt = st[i].ulRunTimeCounter;
    auto it = last.find(st[i].xTaskNumber);  // names are not unique
    uint32_t d = it == last.end() ? 0 : rt - it->second;
    now[st[i].xTaskNumber] = rt;
    if (dt)
      rows.push_back({st[i].pcTaskName, uint32_t(uint64_t(d) * 1000 / dt),
                      st[i].uxCurrentPriority, st[i].xCoreID,
                      (uint32_t)st[i].usStackHighWaterMark});
  }
  last.swap(now);
  if (rows.empty())
    return;
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.pct10 > b.pct10; });
  std::string out = "cpu (% of one core, last 30 s):";
  char b[64];
  for (size_t i = 0; i < rows.size() && i < 10; i++) {
    snprintf(b, sizeof b, " %s=%u.%u(p%u,c%d,s%u)", rows[i].name.c_str(),
             unsigned(rows[i].pct10 / 10), unsigned(rows[i].pct10 % 10),
             unsigned(rows[i].prio), rows[i].core > 1 ? -1 : int(rows[i].core),
             unsigned(rows[i].stack));
    out += b;
  }
  SC32_LOG(info, "%s", out.c_str());
#endif
}

void logHeap() {
  SC32_LOG(info, "heap: internal %u KB free (largest %u KB, min %u KB), psram %u KB free",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void pollSdCard() {
#if CONFIG_SC32_SDCARD
  if (!g.sd)
    return;
  // a removed card must not be unmounted below the SD player
  if (g.sd->isMounted() && !g.sd->cardPresent() && g.streams &&
      g.streams->current() == StreamManager::Service::SD)
    g.streams->stopAll();
  if (g.sd->poll())
    scui_device::notifyPlayback();
#endif
}

// SD hot plug, crash log, statistics (stack in PSRAM: SD card access is
// fine there, flash / NVS work is not done here)
void housekeeping(void*) {
  uint32_t lastHeap = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    pollSdCard();
#if CONFIG_SC32_SDCARD
    crashlog::poll(g.sd && g.sd->isMounted());
#else
    crashlog::poll(false);
#endif
    uint32_t now = nowMs();
    if (now - lastHeap >= 30000) {
      lastHeap = now;
      logHeap();
      logCpu();
    }
  }
}

}  // namespace

void start(const Deps& deps) {
  g = deps;
  stations();  // load the station list now (NVS read on an internal stack)
  if (xTaskCreatePinnedToCoreWithCaps(housekeeping, "housekeeping", 6144, nullptr, 1,
                                      nullptr, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    xTaskCreatePinnedToCore(housekeeping, "housekeeping", 4096, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------- stations --
std::vector<RadioStation> stations() {
  std::lock_guard<std::mutex> lk(g_stMu);
  if (g_stDirty && g.radioStore) {
    g_stDirty = false;
    g_stations.clear();
    Record r;
    g.radioStore->load("stations", &r);
    for (auto& f : r.fields) {
      RadioStation s;
      s.name = f.name;
      s.url = std::string(f.value.begin(), f.value.end());
      g_stations.push_back(std::move(s));
    }
  }
  return g_stations;
}

void notifyStationsChanged() {
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    g_stDirty = true;
  }
#if CONFIG_SC32_WEBSTREAM
  // keep next/previous of a playing radio in sync with the list
  if (g.streams && g.streams->radio &&
      g.streams->current() == StreamManager::Service::Radio)
    g.streams->radio->setStations(stations());
#endif
  scui_device::notifyPlayback();
}

// ------------------------------------------------------------------- SD ----
bool sdAvailable() {
  return g.sd != nullptr;
}
SD_Master* sd() {
  return g.sd;
}

SdInfo sdInfo() {
  SdInfo o;
#if CONFIG_SC32_SDCARD
  if (!g.sd)
    return o;
  auto i = g.sd->info();
  o.present = i.present;
  o.mounted = i.mounted;
  o.name = i.name;
  o.type = i.type;
  o.totalBytes = i.totalBytes;
  o.freeBytes = i.freeBytes;
#endif
  return o;
}

bool sdRemount() {
#if CONFIG_SC32_SDCARD
  if (!g.sd)
    return false;
  // never unmount below an open file
  if (g.streams && g.streams->current() == StreamManager::Service::SD)
    g.streams->stopAll();
  return g.sd->remount() == ESP_OK;
#else
  return false;
#endif
}

// -------------------------------------------------------------- battery ----
Battery battery() {
  Battery b;
#if CONFIG_SC32_BATTERY
  if (!g.battery)
    return b;
  b.millivolts = g.battery->mv();
  b.available = b.millivolts > 0;
  b.percent = std::min(100, int(g.battery->soc() / 10));
  b.milliamps = g.battery->ma();
  b.tempDeciC = g.battery->tc();
#endif
  return b;
}

// -------------------------------------------------------------- network ----
bool wifiConnected() {
  return wifi_manager_is_connected();
}
int wifiRssi() {
  wifi_ap_record_t ap{};
  if (!wifi_manager_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    return 0;
  return ap.rssi;
}
std::string wifiSsid() {
  std::string s;
  wifi_manager_get_connected_ssid(s);
  return s;
}
std::string ipAddress() {
  esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.ip.addr == 0)
    return "-";
  char b[20];
  snprintf(b, sizeof b, IPSTR, IP2STR(&ip.ip));
  return b;
}

// ----------------------------------------------------------------- info ----
std::string fmtBytes(uint64_t b) {
  char s[24];
  if (b >= (1ull << 30))
    snprintf(s, sizeof s, "%.1f GB", double(b) / double(1ull << 30));
  else if (b >= (1ull << 20))
    snprintf(s, sizeof s, "%.1f MB", double(b) / double(1ull << 20));
  else
    snprintf(s, sizeof s, "%u KB", unsigned(b >> 10));
  return s;
}

const char* version() {
  return g.version;
}

std::vector<std::pair<std::string, std::string>> systemInfo() {
  std::vector<std::pair<std::string, std::string>> v;
  char b[48];
  auto& st = sc32::Settings::instance();
  v.push_back({"Version", g.version});
  v.push_back({"Device name", st.deviceName()});
  v.push_back({"mDNS", st.deviceName() + ".local"});
  v.push_back({"IP address", ipAddress()});
  if (wifiConnected()) {
    snprintf(b, sizeof b, "%s %d dBm", wifiSsid().c_str(), wifiRssi());
    v.push_back({"WiFi", b});
  } else {
    v.push_back({"WiFi", "not connected"});
  }
  Battery bt = battery();
  if (bt.available) {
    snprintf(b, sizeof b, "%d %% / %d.%02d V", bt.percent, bt.millivolts / 1000,
             (bt.millivolts % 1000) / 10);
    v.push_back({"Battery", b});
    snprintf(b, sizeof b, "%d mA", bt.milliamps);
    v.push_back({"Current", b});
    snprintf(b, sizeof b, "%d.%d \xC2\xB0" "C", bt.tempDeciC / 10,
             std::abs(bt.tempDeciC % 10));
    v.push_back({"Temperature", b});
  }
  snprintf(b, sizeof b, "%u KB (min %u)",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
  v.push_back({"Free RAM", b});
  snprintf(b, sizeof b, "%u KB", unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  v.push_back({"Free PSRAM", b});
  uint32_t up = nowMs() / 1000;
  snprintf(b, sizeof b, "%uh %02um", unsigned(up / 3600), unsigned(up / 60 % 60));
  v.push_back({"Uptime", b});
  v.push_back({"Last reset", crashlog::lastResetReason()});
  return v;
}

std::vector<std::pair<std::string, std::string>> infoRows() {
  auto rows = systemInfo();
#if CONFIG_SC32_SDCARD
  SdInfo sd = sdInfo();
  rows.push_back({"SD card", sd.mounted ? "mounted" : (sd.present ? "not readable" : "no card")});
  if (sd.mounted) {
    rows.push_back({"SD name", sd.name + (sd.type.empty() ? "" : " (" + sd.type + ")")});
    rows.push_back({"SD size", fmtBytes(sd.totalBytes)});
    rows.push_back({"SD free", fmtBytes(sd.freeBytes)});
  }
#endif
  return rows;
}

}  // namespace device
