// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A minimal Wi-Fi station. On the ESP32-P4, which has no radio, the same
// esp_wifi calls go to the companion chip through esp_wifi_remote.

#include "wifi.h"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "wifi";
constexpr EventBits_t kGotIp = BIT0;
EventGroupHandle_t g_events = nullptr;

void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // The WebSocket client notices on its own and reconnects once we are back.
    xEventGroupClearBits(g_events, kGotIp);
    ESP_LOGW(kTag, "disconnected from \"%s\"; retrying", CONFIG_A2UI_EXAMPLE_WIFI_SSID);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    ESP_LOGI(kTag, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(g_events, kGotIp);
  }
}

esp_err_t init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "nvs erase");
    err = nvs_flash_init();
  }
  return err;
}

// The driver's fields need no terminating NUL when full.
void copy_field(uint8_t* field, size_t size, const char* value) {
  const size_t len = std::strlen(value);
  std::memcpy(field, value, len < size ? len : size);
}

}  // namespace

bool example_wifi_start(uint32_t wait_ms) {
  g_events = xEventGroupCreate();
  ESP_ERROR_CHECK(init_nvs());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));

  wifi_config_t config = {};
  copy_field(config.sta.ssid, sizeof(config.sta.ssid), CONFIG_A2UI_EXAMPLE_WIFI_SSID);
  copy_field(config.sta.password, sizeof(config.sta.password), CONFIG_A2UI_EXAMPLE_WIFI_PASSWORD);
  config.sta.threshold.authmode =
      std::strlen(CONFIG_A2UI_EXAMPLE_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(kTag, "joining \"%s\"", CONFIG_A2UI_EXAMPLE_WIFI_SSID);

  const EventBits_t bits = xEventGroupWaitBits(g_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(wait_ms));
  return (bits & kGotIp) != 0;
}
