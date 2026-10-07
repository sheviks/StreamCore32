#pragma once
// ============================================================================
//  device — device services that exist with or without a display:
//
//  • radio station list (NVS record "stations", cached)
//  • system / SD / battery / WiFi information (web UI, display)
//  • housekeeping task: SD card hot plug, crash log, heap / CPU statistics
//
//  The optional hardware (SD card, battery gauge) may be absent: every
//  function then returns "not available".
// ============================================================================
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "StreamManager.h"  // RadioStation

class SD_Master;
class BQ27220;
class Store;

namespace device {

struct Deps {
  StreamManager* streams = nullptr;
  SD_Master* sd = nullptr;      // null without SD card support
  BQ27220* battery = nullptr;   // null without fuel gauge
  Store* radioStore = nullptr;  // "stations" record: name -> url
  const char* version = "";
};

/** Remember the parts and start the housekeeping task. */
void start(const Deps& deps);

// ---- radio stations ----
std::vector<RadioStation> stations();
/** The station list in the store changed (reload, update the radio). */
void notifyStationsChanged();

// ---- SD card ----
struct SdInfo {
  bool present = false;
  bool mounted = false;
  std::string name, type;
  uint64_t totalBytes = 0, freeBytes = 0;
};
bool sdAvailable();  // compiled in and initialised
SD_Master* sd();
SdInfo sdInfo();
bool sdRemount();

// ---- battery ----
struct Battery {
  bool available = false;
  int percent = 0;
  int millivolts = 0;
  int milliamps = 0;
  int tempDeciC = 0;
};
Battery battery();

// ---- network ----
bool wifiConnected();
int wifiRssi();
std::string wifiSsid();
std::string ipAddress();

// ---- information rows (label, value) ----
std::vector<std::pair<std::string, std::string>> systemInfo();
/** systemInfo() + SD card rows (web UI "System" card). */
std::vector<std::pair<std::string, std::string>> infoRows();

std::string fmtBytes(uint64_t b);
const char* version();

}  // namespace device
