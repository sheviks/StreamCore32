#include "wifi_manager.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char* TAG = "WiFiMgr";

// Event group bits
static constexpr EventBits_t WIFI_CONNECTED_BIT  = BIT0;
static constexpr EventBits_t WIFI_FAIL_BIT       = BIT1;
static constexpr EventBits_t WIFI_CONNECTING_BIT = BIT2;

static EventGroupHandle_t s_wifi_event_group = nullptr;
static SemaphoreHandle_t  s_lock             = nullptr;

static int s_retry_num = 0;
static constexpr int MAX_RETRY = 7;

// Desired credentials (latest request)
static std::string s_target_ssid;
static std::string s_target_pass;
static bool s_have_target = false;

// Generation tokens to ignore stale events and to switch mid-connection cleanly
static uint32_t s_generation        = 0; // increments each user connect request
static uint32_t s_active_generation = 0; // generation currently being attempted

static bool s_wifi_started     = false;
static bool s_manager_enabled  = true;
static bool s_user_disconnect_in_progress = false;

// Callbacks
static wifi_on_connected_t    s_on_connected;
static wifi_on_disconnected_t s_on_disconnected;

// --------------------- NVS helpers ---------------------

static esp_err_t wifi_nvs_save(const std::string& ssid, const std::string& pass) {
  nvs_handle_t h;
  esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_str(h, "ssid", ssid.c_str());
  if (err == ESP_OK) err = nvs_set_str(h, "pass", pass.c_str());
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

static esp_err_t wifi_nvs_load(std::string& ssid_out, std::string& pass_out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open("wifi", NVS_READONLY, &h);
  if (err != ESP_OK) return err;

  size_t ssid_len = 0, pass_len = 0;
  err = nvs_get_str(h, "ssid", nullptr, &ssid_len);
  if (err != ESP_OK) { nvs_close(h); return err; }
  err = nvs_get_str(h, "pass", nullptr, &pass_len);
  if (err != ESP_OK) { nvs_close(h); return err; }

  std::string ssid(ssid_len, '\0');
  std::string pass(pass_len, '\0');

  err = nvs_get_str(h, "ssid", ssid.data(), &ssid_len);
  if (err == ESP_OK) err = nvs_get_str(h, "pass", pass.data(), &pass_len);

  nvs_close(h);
  if (err != ESP_OK) return err;

  // nvs_get_str includes null terminator in len
  if (!ssid.empty() && ssid.back() == '\0') ssid.pop_back();
  if (!pass.empty() && pass.back() == '\0') pass.pop_back();

  ssid_out = ssid;
  pass_out = pass;
  return ESP_OK;
}

// --------------------- Helpers ---------------------

static void snapshot_ssid(char out[33]) {
  std::memset(out, 0, 33);
  if (!s_target_ssid.empty()) {
    std::strncpy(out, s_target_ssid.c_str(), 32);
    out[32] = '\0';
  }
}

// Call callback safely (copy std::function under lock, call after unlock)
static void invoke_disconnected_cb(const char* ssid,
                                   int reason,
                                   bool is_switching,
                                   bool is_user_disconnect) {
  wifi_on_disconnected_t cb;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  cb = s_on_disconnected;
  xSemaphoreGive(s_lock);

  if (cb) cb(ssid, reason, is_switching, is_user_disconnect);
}

static void invoke_connected_cb(const char* ssid, esp_ip4_addr_t ip) {
  wifi_on_connected_t cb;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  cb = s_on_connected;
  xSemaphoreGive(s_lock);

  if (cb) cb(ssid, ip);
}

static void start_connect_locked() {
  if (!s_manager_enabled) return;
  if (!s_have_target) {
    ESP_LOGW(TAG, "No target SSID set");
    return;
  }
  if (!s_wifi_started) {
    ESP_LOGW(TAG, "WiFi not started yet");
    return;
  }

  wifi_config_t cfg{};
  std::memset(&cfg, 0, sizeof(cfg));
  std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid),
               s_target_ssid.c_str(),
               sizeof(cfg.sta.ssid));
  std::strncpy(reinterpret_cast<char*>(cfg.sta.password),
               s_target_pass.c_str(),
               sizeof(cfg.sta.password));

  // PMF default is fine; adjust as needed
  cfg.sta.pmf_cfg.capable  = true;
  cfg.sta.pmf_cfg.required = false;

  // NOTE: If you set threshold.authmode too strict, some networks may fail.
  // Leave it default unless you specifically want to disallow open/WEP.
  // cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  s_active_generation = s_generation;
  s_retry_num = 0;

  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTING_BIT);

  // Disconnect first (ignore "not connected" errors)
  (void)esp_wifi_disconnect();
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Connecting to SSID='%s' (gen=%lu)",
             s_target_ssid.c_str(),
             static_cast<unsigned long>(s_active_generation));
  }
}

// --------------------- Event handler ---------------------

static void event_handler(void* /*arg*/,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void* event_data) {
  if (!s_wifi_event_group || !s_lock) return;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    start_connect_locked();
    xSemaphoreGive(s_lock);
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* disc = reinterpret_cast<wifi_event_sta_disconnected_t*>(event_data);
    const int reason = disc ? static_cast<int>(disc->reason) : -1;

    char ssid_snap[33];
    bool is_switching = false;
    bool is_user_disc = false;

    // Decide behavior under lock
    xSemaphoreTake(s_lock, portMAX_DELAY);

    snapshot_ssid(ssid_snap);

    is_user_disc = s_user_disconnect_in_progress;

    if (!s_manager_enabled) {
      // Manager disabled; don't auto-retry, but still notify.
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      xSemaphoreGive(s_lock);

      invoke_disconnected_cb(ssid_snap, reason, false, is_user_disc);
      return;
    }

    // If a newer request exists, switch immediately and don’t retry old one.
    if (s_active_generation != s_generation) {
      is_switching = true;
      ESP_LOGI(TAG, "Disconnected (reason=%d). Switching to latest target (gen=%lu).",
               reason, static_cast<unsigned long>(s_generation));
      start_connect_locked();
      xSemaphoreGive(s_lock);

      invoke_disconnected_cb(ssid_snap, reason, true, is_user_disc);
      return;
    }

    // Normal bounded retry
    if (s_retry_num < MAX_RETRY) {
      s_retry_num++;
      ESP_LOGW(TAG, "Disconnected (reason=%d). Retry %d/%d ...",
               reason, s_retry_num, MAX_RETRY);
      (void)esp_wifi_connect();
      xSemaphoreGive(s_lock);

      invoke_disconnected_cb(ssid_snap, reason, false, is_user_disc);
      return;
    }

    ESP_LOGE(TAG, "Failed to connect after %d retries (reason=%d).",
             MAX_RETRY, reason);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTING_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

    xSemaphoreGive(s_lock);

    invoke_disconnected_cb(ssid_snap, reason, false, is_user_disc);
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto* event = reinterpret_cast<ip_event_got_ip_t*>(event_data);
    if (!event) return;

    char ssid_snap[33];
    bool stale = false;
    esp_ip4_addr_t ip = event->ip_info.ip;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot_ssid(ssid_snap);

    if (!s_manager_enabled) {
      xSemaphoreGive(s_lock);
      return;
    }

    // If this IP is for an old generation, force switch to newest.
    if (s_active_generation != s_generation) {
      stale = true;
      ESP_LOGI(TAG, "Got IP for stale generation (active=%lu, current=%lu). Forcing switch.",
               static_cast<unsigned long>(s_active_generation),
               static_cast<unsigned long>(s_generation));
      (void)esp_wifi_disconnect();
      start_connect_locked();
      xSemaphoreGive(s_lock);
      return;
    }

    // Connected for the current generation
    s_retry_num = 0;
    s_user_disconnect_in_progress = false;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTING_BIT | WIFI_FAIL_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    xSemaphoreGive(s_lock);

    if (!stale) {
      ESP_LOGI(TAG, "Got IP: " IPSTR " (SSID='%s')",
               IP2STR(&ip), ssid_snap);
      invoke_connected_cb(ssid_snap, ip);
    }
    return;
  }
}

// --------------------- Public API ---------------------

esp_err_t wifi_manager_init_sta() {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
  if (!s_lock || !s_wifi_event_group) return ESP_ERR_NO_MEM;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  s_wifi_started = true;

  ESP_LOGI(TAG, "wifi_manager_init_sta done");
  return ESP_OK;
}

void wifi_manager_set_callbacks(wifi_on_connected_t on_connected,
                                wifi_on_disconnected_t on_disconnected) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_on_connected = std::move(on_connected);
  s_on_disconnected = std::move(on_disconnected);
  xSemaphoreGive(s_lock);
}

esp_err_t wifi_manager_connect(const std::string& ssid,
                               const std::string& pass,
                               bool persist_to_nvs) {
  if (ssid.empty()) return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(s_lock, portMAX_DELAY);

  s_target_ssid = ssid;
  s_target_pass = pass;
  s_have_target = true;

  s_manager_enabled = true;
  s_user_disconnect_in_progress = false;

  s_generation++;  // invalidate old attempt events/retries
  s_retry_num = 0;

  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTING_BIT);

  if (persist_to_nvs) {
    esp_err_t err = wifi_nvs_save(ssid, pass);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed saving creds to NVS: %s", esp_err_to_name(err));
      // keep going anyway
    }
  }

  start_connect_locked();

  xSemaphoreGive(s_lock);
  return ESP_OK;
}

esp_err_t wifi_manager_connect_from_nvs() {
  std::string ssid, pass;
  esp_err_t err = wifi_nvs_load(ssid, pass);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No saved WiFi creds in NVS (%s).", esp_err_to_name(err));
    return err;
  }
  return wifi_manager_connect(ssid, pass, false);
}

esp_err_t wifi_manager_get_connected_ssid(std::string& ssid_out) {
  if (!s_lock) return ESP_ERR_WIFI_NOT_INIT;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!wifi_manager_is_connected()) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_WIFI_NOT_CONNECT;
  }
  ssid_out = s_target_ssid;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

esp_err_t wifi_manager_get_connected_pass(std::string& pass_out) {
  if (!s_lock) return ESP_ERR_WIFI_NOT_INIT;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!wifi_manager_is_connected()) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_WIFI_NOT_CONNECT;
  }
  pass_out = s_target_pass;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

void wifi_manager_disconnect(bool stop_wifi_driver) {
  xSemaphoreTake(s_lock, portMAX_DELAY);

  s_user_disconnect_in_progress = true;
  s_manager_enabled = false;
  s_generation++;        // invalidate events from previous connect
  s_have_target = false;

  xEventGroupClearBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT);
  xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

  (void)esp_wifi_disconnect();

  if (stop_wifi_driver && s_wifi_started) {
    (void)esp_wifi_stop();
    s_wifi_started = false;
  }

  xSemaphoreGive(s_lock);
}

void wifi_manager_enable() {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_manager_enabled = true;
  s_user_disconnect_in_progress = false;
  xSemaphoreGive(s_lock);
}

bool wifi_manager_wait(uint32_t timeout_ms) {
  if (!s_wifi_event_group) return false;
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group,
      WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
      pdFALSE, pdFALSE,
      timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));

  return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected() {
  if (!s_wifi_event_group) return false;  // wifi_manager_init_sta() not called yet
  const EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
  return (bits & WIFI_CONNECTED_BIT) != 0;
}
