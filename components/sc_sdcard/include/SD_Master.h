#pragma once
// ============================================================================
//  SD_Master — SD card (SDMMC, 1-bit) mounted as FAT at /sdcard.
//
//  • mount() / unmount() / remount()
//  • card detect pin (CONFIG_GPIO_SD_CD, active low; < 0 = no CD pin)
//  • poll(): call every few seconds — mounts a freshly inserted card and
//    unmounts a removed one (returns true when the state changed)
//  • info(): name, capacity and free space for the UI
//
//  All methods are thread safe.
// ============================================================================
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "sdmmc_cmd.h"

class SD_Master {
 public:
  static constexpr const char* kMountPoint = "/sdcard";

  struct Info {
    bool present = false;  // card detect (true when there is no CD pin)
    bool mounted = false;
    std::string name;       // card product name, e.g. "SC32G"
    std::string type;       // "SDHC", "SDXC", "SDSC", "MMC"
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint32_t speedKhz = 0;
    esp_err_t lastError = ESP_OK;
  };

  SD_Master() = default;
  ~SD_Master();

  /** Mount the card (no-op when mounted). */
  esp_err_t init() { return mount(); }
  esp_err_t mount();
  void unmount();
  esp_err_t remount();

  bool isMounted() const { return mounted_.load(); }
  /** Card detect state; true if no CD pin is configured. */
  bool cardPresent() const;

  /** Hot-plug handling. Returns true when the mount state changed. */
  bool poll();

  Info info();
  const char* mountPoint() const { return kMountPoint; }

  sdmmc_card_t* card_ = nullptr;

 private:
  std::recursive_mutex mu_;
  std::atomic<bool> mounted_{false};
  esp_err_t lastError_ = ESP_OK;
  bool cdConfigured_ = false;
};
