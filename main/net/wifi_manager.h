#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Wi-Fi manager.
 *
 * Joins the saved Wi-Fi network.  When nothing is saved, or the saved network
 * cannot be reached, it opens a setup access point (CONFIG_WIFI_AP_SSID,
 * "WebRadio" by default) with a captive portal: a dark, minimal web page where
 * the user picks a network and enters the password.  Credentials are stored in
 * NVS.  The same page stays available on the device's IP address once it is on
 * the home network, to change or forget the network.
 */

typedef enum {
  WIFI_MANAGER_PORTAL,     /* setup access point open, waiting for the user */
  WIFI_MANAGER_CONNECTING, /* joining a network */
  WIFI_MANAGER_CONNECTED,  /* has an IP address */
} wifi_manager_state_t;

/**
 * Initialise NVS, the network stack and Wi-Fi, start the web page and begin
 * connecting.  Returns once started; it does not wait for a connection.
 */
esp_err_t wifi_manager_start(void);

/**
 * @return true while associated and holding an IP address
 */
bool wifi_manager_is_connected(void);

/**
 * @return true while the setup access point is up
 */
bool wifi_manager_portal_active(void);

wifi_manager_state_t wifi_manager_get_state(void);
const char *wifi_manager_state_name(wifi_manager_state_t state);
