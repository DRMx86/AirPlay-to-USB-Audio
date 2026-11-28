#include "wifi_sta.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <string.h>

#define TAG "wifi_sta"

#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *ev = data;
    xEventGroupClearBits(s_events, CONNECTED_BIT);
    ESP_LOGW(TAG, "Disconnected (reason %d), reconnecting", ev->reason);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, CONNECTED_BIT);
  }
}

esp_err_t wifi_sta_start(void) {
  if (strlen(CONFIG_RADIO_WIFI_SSID) == 0) {
    ESP_LOGE(TAG, "No Wi-Fi SSID set. Configure it in menuconfig: Web Radio to "
                  "USB DAC Audio -> Wi-Fi");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
    err = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

  s_events = xEventGroupCreate();
  if (!s_events) {
    return ESP_ERR_NO_MEM;
  }

  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
      TAG, "wifi handler failed");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL),
      TAG, "ip handler failed");

  wifi_config_t cfg = {0};
  strlcpy((char *)cfg.sta.ssid, CONFIG_RADIO_WIFI_SSID, sizeof(cfg.sta.ssid));
  strlcpy((char *)cfg.sta.password, CONFIG_RADIO_WIFI_PASSWORD,
          sizeof(cfg.sta.password));
  if (strlen(CONFIG_RADIO_WIFI_PASSWORD) > 0) {
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG,
                      "set config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
  /* Modem sleep adds latency spikes that starve a live stream. */
  esp_wifi_set_ps(WIFI_PS_NONE);

  ESP_LOGI(TAG, "Connecting to \"%s\"", CONFIG_RADIO_WIFI_SSID);
  return ESP_OK;
}

bool wifi_sta_is_connected(void) {
  return s_events && (xEventGroupGetBits(s_events) & CONNECTED_BIT);
}

bool wifi_sta_wait_connected(uint32_t timeout_ms) {
  if (!s_events) {
    return false;
  }
  EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE,
                                         pdTRUE, pdMS_TO_TICKS(timeout_ms));
  return bits & CONNECTED_BIT;
}
