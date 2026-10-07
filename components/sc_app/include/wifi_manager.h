#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"
#include "esp_netif.h"  // esp_ip4_addr_t

// Bits / state are internal; public API is below.

using wifi_on_connected_t =
    std::function<void(const char* ssid, esp_ip4_addr_t ip)>;

// reason = WIFI_REASON_* (from esp_wifi_types.h / WiFi events)
// is_switching = true when a disconnect happens because a newer connect request replaced the old one
// is_user_disconnect = true when wifi_manager_disconnect() initiated shutdown
using wifi_on_disconnected_t =
    std::function<void(const char* ssid, int reason, bool is_switching, bool is_user_disconnect)>;

// Initialize STA + event handlers + start WiFi driver.
esp_err_t wifi_manager_init_sta();

// Set callbacks (either can be empty/null).
void wifi_manager_set_callbacks(wifi_on_connected_t on_connected,
                                wifi_on_disconnected_t on_disconnected);

// Connect to given SSID/PASS. If another attempt is in progress, it switches.
// persist_to_nvs: store ssid/pass in NVS namespace "wifi" keys "ssid"/"pass".
esp_err_t wifi_manager_connect(const std::string& ssid,
                               const std::string& pass,
                               bool persist_to_nvs);

// Load ssid/pass from NVS (namespace "wifi") and connect.
// Returns error if missing/invalid.
esp_err_t wifi_manager_connect_from_nvs();

esp_err_t wifi_manager_get_connected_ssid(std::string& ssid_out);

esp_err_t wifi_manager_get_connected_pass(std::string& pass_out);

// Disconnect. If stop_wifi_driver=true, stops WiFi driver too.
void wifi_manager_disconnect(bool stop_wifi_driver = false);

// Re-enable internal auto-management if you previously disconnected/stopped.
void wifi_manager_enable();

// Wait until connected or failed.
// timeout_ms: use UINT32_MAX to wait forever.
// Returns true if connected.
bool wifi_manager_wait(uint32_t timeout_ms);

// Returns current connected bit.
bool wifi_manager_is_connected();
