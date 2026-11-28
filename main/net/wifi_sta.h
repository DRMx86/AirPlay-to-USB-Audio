#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Wi-Fi station.  Credentials come from CONFIG_RADIO_WIFI_SSID and
 * CONFIG_RADIO_WIFI_PASSWORD.  Reconnects automatically if the link drops.
 */

/**
 * Initialise NVS, the network stack and Wi-Fi, and start connecting.
 * Returns immediately; use wifi_sta_is_connected() to see when an IP arrived.
 *
 * @return ESP_ERR_INVALID_STATE if no SSID has been configured
 */
esp_err_t wifi_sta_start(void);

/**
 * @return true while associated and holding an IP address
 */
bool wifi_sta_is_connected(void);

/**
 * Block until connected or the timeout expires.
 */
bool wifi_sta_wait_connected(uint32_t timeout_ms);
