#include "SD_Master.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "Logger.h"

SD_Master::~SD_Master() {
  unmount();
}

bool SD_Master::cardPresent() const {
#if defined(CONFIG_GPIO_SD_CD) && (CONFIG_GPIO_SD_CD >= 0)
  // CD switch closes to GND when a card is inserted
  return gpio_get_level((gpio_num_t)CONFIG_GPIO_SD_CD) == 0;
#else
  return true;
#endif
}

esp_err_t SD_Master::mount() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (mounted_.load())
    return ESP_OK;

#if defined(CONFIG_GPIO_SD_CD) && (CONFIG_GPIO_SD_CD >= 0)
  if (!cdConfigured_) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_GPIO_SD_CD;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    cdConfigured_ = true;
  }
  if (!cardPresent()) {
    lastError_ = ESP_ERR_NOT_FOUND;
    SC32_LOG(info, "SD: no card inserted");
    return lastError_;
  }
#endif

  SC32_LOG(info, "SD: mounting card at %s", kMountPoint);
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
  mount_config.format_if_mount_failed = true;
#else
  mount_config.format_if_mount_failed = false;
#endif
  mount_config.max_files = 8;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
#if defined(CONFIG_GPIO_SD_CD) && (CONFIG_GPIO_SD_CD >= 0)
  slot_config.gpio_cd = (gpio_num_t)CONFIG_GPIO_SD_CD;
#endif
  slot_config.width = 1;
  slot_config.clk = (gpio_num_t)CONFIG_GPIO_SD_CLK;
  slot_config.cmd = (gpio_num_t)CONFIG_GPIO_SD_CMD;
  slot_config.d0 = (gpio_num_t)CONFIG_GPIO_SD_D0;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_card_t* card = nullptr;
  esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config,
                                          &mount_config, &card);
  lastError_ = err;
  if (err != ESP_OK) {
    SC32_LOG(error, "SD: mount failed: %s", esp_err_to_name(err));
    card_ = nullptr;
    return err;
  }
  card_ = card;
  mounted_.store(true);
  sdmmc_card_print_info(stdout, card);
  return ESP_OK;
}

void SD_Master::unmount() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!mounted_.load())
    return;
  esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
  card_ = nullptr;
  mounted_.store(false);
  SC32_LOG(info, "SD: unmounted");
}

esp_err_t SD_Master::remount() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  unmount();
  return mount();
}

bool SD_Master::poll() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
#if defined(CONFIG_GPIO_SD_CD) && (CONFIG_GPIO_SD_CD >= 0)
  const bool present = cardPresent();
  if (mounted_.load() && !present) {
    SC32_LOG(info, "SD: card removed");
    unmount();
    return true;
  }
  if (!mounted_.load() && present) {
    vTaskDelay(pdMS_TO_TICKS(300));  // contact bounce while inserting
    return mount() == ESP_OK;
  }
#endif
  return false;
}

SD_Master::Info SD_Master::info() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Info i;
  i.present = cardPresent();
  i.mounted = mounted_.load();
  i.lastError = lastError_;
  if (!i.mounted || !card_)
    return i;
  i.name = std::string(card_->cid.name);
  if (card_->is_mmc)
    i.type = "MMC";
  else if (card_->ocr & (1 << 30))  // SDHC/SDXC (high capacity)
    i.type = (uint64_t)card_->csd.capacity * card_->csd.sector_size >
                     (32ULL << 30)
                 ? "SDXC"
                 : "SDHC";
  else
    i.type = "SDSC";
  i.speedKhz = card_->max_freq_khz;
  uint64_t total = 0, freeB = 0;
  if (esp_vfs_fat_info(kMountPoint, &total, &freeB) == ESP_OK) {
    i.totalBytes = total;
    i.freeBytes = freeB;
  } else {
    i.totalBytes = (uint64_t)card_->csd.capacity * card_->csd.sector_size;
  }
  return i;
}
