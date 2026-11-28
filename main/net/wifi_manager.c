/**
 * Wi-Fi manager: saved network first, setup portal when that fails.
 *
 *   boot -> saved network? -- yes --> connect (retry) --> up
 *              | no                        | gives up
 *              v                           v
 *        setup AP "WebRadio" + captive DNS + web page  <-- user picks a network
 *
 * The web server stays up on the home network too, so the network can be
 * changed or forgotten later at http://<device ip>/.
 *
 * Threads: Wi-Fi/IP events run on the default event loop task, HTTP handlers on
 * the httpd task, timers on the timer service task.  Shared state is either a
 * single volatile word or guarded by s_lock.
 */

#include "wifi_manager.h"

#include "audio_output.h"
#include "captive_dns.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_player.h"
#include "stations.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wifi_mgr"

#define NVS_NAMESPACE "wifi"
#define HOSTNAME      "webradio"

#define MAX_NETWORKS     30
#define NEW_ATTEMPTS     2 /* tries for a network typed into the page */
#define PORTAL_LINGER_MS 15000
#define SAVED_RETRY_MS   (5 * 60 * 1000)
#define BODY_MAX         256

void radio_player_select(size_t index); /* radio_player.c */
void radio_player_set_paused(bool paused);
bool radio_player_is_paused(void);
static void mdns_start_once(void);
static void log_capture_start(void);

extern const char portal_start[] asm("_binary_portal_html_start");
extern const char portal_end[] asm("_binary_portal_html_end");

typedef struct {
  char ssid[33];
  int8_t rssi;
  bool secure;
} net_t;

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static TimerHandle_t s_linger_timer;
static TimerHandle_t s_retry_timer;

/* Saved credentials (also in NVS). */
static char s_ssid[33];
static char s_pass[65];
static bool s_have_creds;

/* Credentials being tried from the web page. */
static char s_new_ssid[33];
static char s_new_pass[65];
static volatile bool s_trying_new;
static int s_new_attempts;

static volatile bool s_vol_touched; /* the page changed the volume */
static volatile bool s_sta_wanted; /* keep (re)connecting the station */
static volatile bool s_connected;
static volatile bool s_portal;
static volatile int s_ap_clients;
static int s_retries;
static bool s_was_connected; /* connected at least once since boot */
static volatile wifi_manager_state_t s_state = WIFI_MANAGER_CONNECTING;
static char s_ip[16];
static char s_ap_ip[16] = "192.168.4.1";

/* Outcome of the latest "connect" request from the page. */
static uint32_t s_attempt;
static int s_result; /* 0 pending, 1 connected, 2 failed */
static char s_result_msg[64];

/* Scan cache. */
static net_t s_nets[MAX_NETWORKS];
static int s_net_count;
static volatile bool s_scanning;

/* ── Small helpers ───────────────────────────────────────────────────────── */

static void lock(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
  xSemaphoreGive(s_lock);
}

static const char *reason_text(uint8_t reason) {
  switch (reason) {
  case WIFI_REASON_AUTH_FAIL:
  case WIFI_REASON_AUTH_EXPIRE:
  case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
  case WIFI_REASON_HANDSHAKE_TIMEOUT:
    return "Wrong password";
  case WIFI_REASON_NO_AP_FOUND:
    return "Network not found";
  default:
    return "Could not connect";
  }
}

const char *wifi_manager_state_name(wifi_manager_state_t state) {
  switch (state) {
  case WIFI_MANAGER_PORTAL:
    return "portal";
  case WIFI_MANAGER_CONNECTING:
    return "connecting";
  case WIFI_MANAGER_CONNECTED:
    return "connected";
  }
  return "unknown";
}

/* ── NVS ─────────────────────────────────────────────────────────────────── */

static void creds_load(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    return;
  }
  size_t n = sizeof(s_ssid);
  if (nvs_get_str(h, "ssid", s_ssid, &n) == ESP_OK && s_ssid[0]) {
    s_have_creds = true;
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) {
      s_pass[0] = '\0';
    }
  } else {
    s_ssid[0] = '\0';
  }
  nvs_close(h);
}

static esp_err_t creds_save(const char *ssid, const char *pass) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_str(h, "ssid", ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(h, "pass", pass);
  }
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

static void creds_erase(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  nvs_erase_key(h, "ssid");
  nvs_erase_key(h, "pass");
  nvs_commit(h);
  nvs_close(h);
}

/* ── Station / portal control ────────────────────────────────────────────── */

static void sta_apply_config(const char *ssid, const char *pass) {
  wifi_config_t cfg = {0};
  strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
  strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
  cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;
  cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void scan_start(void) {
  if (s_scanning) {
    return;
  }
  wifi_scan_config_t sc = {
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active = {.min = 100, .max = 200},
  };
  /* Fails while the station is mid-connect; the cached list is still served. */
  if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
    s_scanning = true;
  }
}

static void scan_collect(void) {
  s_scanning = false;
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n == 0) {
    esp_wifi_clear_ap_list();
    lock();
    s_net_count = 0;
    unlock();
    return;
  }
  if (n > MAX_NETWORKS) {
    n = MAX_NETWORKS;
  }
  wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
  if (!recs) {
    esp_wifi_clear_ap_list();
    return;
  }
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
    free(recs);
    return;
  }

  lock();
  s_net_count = 0;
  for (uint16_t i = 0; i < n; i++) {
    const char *ssid = (const char *)recs[i].ssid;
    if (ssid[0] == '\0') {
      continue; /* hidden network */
    }
    int at = -1;
    for (int j = 0; j < s_net_count; j++) {
      if (strcmp(s_nets[j].ssid, ssid) == 0) {
        at = j;
        break;
      }
    }
    if (at >= 0) { /* same name on several access points: keep the strongest */
      if (recs[i].rssi > s_nets[at].rssi) {
        s_nets[at].rssi = recs[i].rssi;
      }
      continue;
    }
    net_t *e = &s_nets[s_net_count++];
    strlcpy(e->ssid, ssid, sizeof(e->ssid));
    e->rssi = recs[i].rssi;
    e->secure = recs[i].authmode != WIFI_AUTH_OPEN;
  }
  for (int i = 1; i < s_net_count; i++) { /* strongest first */
    net_t key = s_nets[i];
    int j = i - 1;
    while (j >= 0 && s_nets[j].rssi < key.rssi) {
      s_nets[j + 1] = s_nets[j];
      j--;
    }
    s_nets[j + 1] = key;
  }
  unlock();
  free(recs);
}

static void portal_open(void) {
  /* Whatever the station was doing, stop it so scans and the AP are stable. */
  s_sta_wanted = false;
  esp_wifi_disconnect();

  if (!s_portal) {
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_WIFI_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    if (strlen(CONFIG_WIFI_AP_PASSWORD) >= 8) {
      strlcpy((char *)ap.ap.password, CONFIG_WIFI_AP_PASSWORD,
              sizeof(ap.ap.password));
      ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
      ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
      esp_ip4addr_ntoa(&ip.ip, s_ap_ip, sizeof(s_ap_ip));
      captive_dns_start(ip.ip.addr);
    }
    s_portal = true;
    ESP_LOGI(TAG, "Setup portal open: join Wi-Fi \"%s\" and go to http://%s/",
             CONFIG_WIFI_AP_SSID, s_ap_ip);
  }
  s_state = WIFI_MANAGER_PORTAL;
  scan_start();

  if (s_have_creds && s_retry_timer) {
    xTimerStart(s_retry_timer, 0); /* try the saved network again later */
  }
}

static void portal_close(void) {
  if (!s_portal) {
    return;
  }
  captive_dns_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  s_portal = false;
  s_ap_clients = 0;
  if (s_retry_timer) {
    xTimerStop(s_retry_timer, 0);
  }
  ESP_LOGI(TAG, "Setup portal closed");
}

static void linger_cb(TimerHandle_t t) {
  (void)t;
  if (s_connected) {
    portal_close();
  }
}

static void retry_cb(TimerHandle_t t) {
  (void)t;
  if (s_connected || s_trying_new || s_ap_clients > 0 || !s_have_creds) {
    return; /* someone is using the portal, or already online */
  }
  ESP_LOGI(TAG, "Retrying saved network \"%s\"", s_ssid);
  s_retries = 0;
  sta_apply_config(s_ssid, s_pass);
  s_sta_wanted = true;
  s_state = WIFI_MANAGER_CONNECTING;
  esp_wifi_connect();
}

static void new_attempt_done(bool ok, const char *msg) {
  s_trying_new = false;
  lock();
  s_result = ok ? 1 : 2;
  strlcpy(s_result_msg, msg, sizeof(s_result_msg));
  unlock();
  if (ok) {
    return;
  }
  ESP_LOGW(TAG, "Could not join \"%s\": %s", s_new_ssid, msg);
  if (s_have_creds) { /* go back to the network that used to work */
    sta_apply_config(s_ssid, s_pass);
    s_retries = 0;
    s_sta_wanted = true;
    s_state = WIFI_MANAGER_CONNECTING;
    esp_wifi_connect();
  } else {
    portal_open();
  }
}

/* ── Wi-Fi / IP events ───────────────────────────────────────────────────── */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    switch (id) {
    case WIFI_EVENT_STA_START:
      if (s_sta_wanted) {
        esp_wifi_connect();
      }
      break;

    case WIFI_EVENT_STA_DISCONNECTED: {
      const wifi_event_sta_disconnected_t *ev = data;
      s_connected = false;
      s_ip[0] = '\0';
      if (!s_sta_wanted) {
        break;
      }
      if (s_trying_new) {
        if (ev->reason == WIFI_REASON_ASSOC_LEAVE) {
          break; /* our own disconnect() before switching networks */
        }
        if (++s_new_attempts < NEW_ATTEMPTS) {
          esp_wifi_connect();
        } else {
          new_attempt_done(false, reason_text(ev->reason));
        }
        break;
      }
      ESP_LOGW(TAG, "Disconnected (reason %d)", ev->reason);
      s_retries++;
      if (!s_was_connected && s_retries >= CONFIG_WIFI_MAX_RETRY) {
        ESP_LOGW(TAG, "Saved network unreachable, opening setup portal");
        portal_open();
        break;
      }
      s_state = WIFI_MANAGER_CONNECTING;
      esp_wifi_connect();
      break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
      s_ap_clients++;
      break;

    case WIFI_EVENT_AP_STADISCONNECTED:
      if (s_ap_clients > 0) {
        s_ap_clients--;
      }
      break;

    case WIFI_EVENT_SCAN_DONE:
      scan_collect();
      break;

    default:
      break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *ev = data;
    esp_ip4addr_ntoa(&ev->ip_info.ip, s_ip, sizeof(s_ip));
    ESP_LOGI(TAG, "Connected, IP %s", s_ip);
    s_connected = true;
    s_state = WIFI_MANAGER_CONNECTED;
    s_retries = 0;
    s_was_connected = true;
    mdns_start_once();

    if (s_trying_new) {
      esp_err_t err = creds_save(s_new_ssid, s_new_pass);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not save credentials: %s", esp_err_to_name(err));
      }
      lock();
      strlcpy(s_ssid, s_new_ssid, sizeof(s_ssid));
      strlcpy(s_pass, s_new_pass, sizeof(s_pass));
      unlock();
      s_have_creds = true;
      new_attempt_done(true, "");
    }
    if (s_portal && s_linger_timer) {
      /* Leave the page up briefly so the phone can read the result. */
      xTimerStart(s_linger_timer, 0);
    }
  }
}

/* ── JSON output ─────────────────────────────────────────────────────────── */

typedef struct {
  char *p;
  size_t cap;
  size_t len;
} jbuf_t;

static void j_raw(jbuf_t *j, const char *s) {
  size_t n = strlen(s);
  if (j->len + n < j->cap) {
    memcpy(j->p + j->len, s, n);
    j->len += n;
    j->p[j->len] = '\0';
  }
}

static void j_str(jbuf_t *j, const char *s) {
  char tmp[8];
  j_raw(j, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      tmp[0] = '\\';
      tmp[1] = (char)c;
      tmp[2] = '\0';
    } else if (c < 0x20) {
      snprintf(tmp, sizeof(tmp), "\\u%04x", c);
    } else {
      tmp[0] = (char)c;
      tmp[1] = '\0';
    }
    j_raw(j, tmp);
  }
  j_raw(j, "\"");
}

static void j_int(jbuf_t *j, int v) {
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%d", v);
  j_raw(j, tmp);
}

static esp_err_t send_json(httpd_req_t *req, const jbuf_t *j) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, j->p, (ssize_t)j->len);
}

/* ── HTTP handlers ───────────────────────────────────────────────────────── */

static esp_err_t root_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, portal_start,
                         (ssize_t)(portal_end - portal_start));
}

static esp_err_t networks_get(httpd_req_t *req) {
  if (strstr(req->uri, "refresh=1")) {
    scan_start();
  }
  jbuf_t j = {.p = malloc(MAX_NETWORKS * 90 + 64),
              .cap = MAX_NETWORKS * 90 + 64};
  if (!j.p) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
  }
  j.p[0] = '\0';
  j_raw(&j, "{\"scanning\":");
  j_raw(&j, s_scanning ? "true" : "false");
  j_raw(&j, ",\"networks\":[");
  lock();
  for (int i = 0; i < s_net_count; i++) {
    if (i) {
      j_raw(&j, ",");
    }
    j_raw(&j, "{\"ssid\":");
    j_str(&j, s_nets[i].ssid);
    j_raw(&j, ",\"rssi\":");
    j_int(&j, s_nets[i].rssi);
    j_raw(&j, ",\"secure\":");
    j_raw(&j, s_nets[i].secure ? "true" : "false");
    j_raw(&j, "}");
  }
  unlock();
  j_raw(&j, "]}");
  esp_err_t err = send_json(req, &j);
  free(j.p);
  return err;
}

static esp_err_t status_get(httpd_req_t *req) {
  char buf[560];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';

  audio_output_stats_t out;
  audio_output_get_stats(&out);

  int rssi = 0;
  if (s_connected) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      rssi = ap.rssi;
    }
  }

  j_raw(&j, "{\"state\":");
  j_str(&j, wifi_manager_state_name(s_state));
  j_raw(&j, ",\"portal\":");
  j_raw(&j, s_portal ? "true" : "false");
  j_raw(&j, ",\"ap\":");
  j_str(&j, CONFIG_WIFI_AP_SSID);
  lock();
  j_raw(&j, ",\"ssid\":");
  j_str(&j, s_connected ? s_ssid : "");
  j_raw(&j, ",\"saved\":");
  j_str(&j, s_have_creds ? s_ssid : "");
  j_raw(&j, ",\"ip\":");
  j_str(&j, s_ip);
  j_raw(&j, ",\"rssi\":");
  j_int(&j, rssi);
  j_raw(&j, ",\"volume\":");
  j_int(&j, out.volume_percent);
  j_raw(&j, ",\"hw\":");
  j_raw(&j, out.hw_volume ? "true" : "false");
  j_raw(&j, ",\"dac\":");
  j_raw(&j, out.connected ? "true" : "false");
  j_raw(&j, ",\"attempt\":");
  j_int(&j, (int)s_attempt);
  j_raw(&j, ",\"result\":");
  j_str(&j, s_result == 1 ? "ok" : (s_result == 2 ? "fail" : "pending"));
  j_raw(&j, ",\"error\":");
  j_str(&j, s_result_msg);
  unlock();
  j_raw(&j, "}");
  return send_json(req, &j);
}

/* Value of `key` in an application/x-www-form-urlencoded body.
 * Returns 1 if found, 0 if absent, -1 if it does not fit in `out`. */
static int form_get(const char *body, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  const char *p = body;
  out[0] = '\0';
  while (*p) {
    const char *amp = strchr(p, '&');
    const char *end = amp ? amp : p + strlen(p);
    const char *eq = memchr(p, '=', (size_t)(end - p));
    if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
      size_t o = 0;
      for (const char *q = eq + 1; q < end; q++) {
        char c = *q;
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && q + 2 < end && isxdigit((unsigned char)q[1]) &&
                   isxdigit((unsigned char)q[2])) {
          char hex[3] = {q[1], q[2], '\0'};
          c = (char)strtol(hex, NULL, 16);
          q += 2;
        }
        if (o + 1 >= cap) {
          out[0] = '\0';
          return -1;
        }
        out[o++] = c;
      }
      out[o] = '\0';
      return 1;
    }
    if (!amp) {
      break;
    }
    p = amp + 1;
  }
  return 0;
}

static esp_err_t connect_post(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len >= BODY_MAX) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
  }
  char body[BODY_MAX];
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, body + got, req->content_len - got);
    if (n <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    got += (size_t)n;
  }
  body[got] = '\0';

  char ssid[33];
  char pass[65];
  if (form_get(body, "ssid", ssid, sizeof(ssid)) != 1 || ssid[0] == '\0' ||
      form_get(body, "pass", pass, sizeof(pass)) < 0) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid");
  }
  size_t plen = strlen(pass);
  if (plen != 0 && (plen < 8 || plen > 63)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad password");
  }
  if (s_trying_new) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "busy");
  }

  lock();
  strlcpy(s_new_ssid, ssid, sizeof(s_new_ssid));
  strlcpy(s_new_pass, pass, sizeof(s_new_pass));
  s_attempt++;
  s_result = 0;
  s_result_msg[0] = '\0';
  uint32_t attempt = s_attempt;
  unlock();

  s_new_attempts = 0;
  s_trying_new = true;
  ESP_LOGI(TAG, "Trying \"%s\"", ssid);

  esp_wifi_scan_stop(); /* a running scan blocks connecting */
  s_scanning = false;
  sta_apply_config(ssid, pass);
  s_sta_wanted = true;
  s_state = WIFI_MANAGER_CONNECTING;
  esp_wifi_disconnect(); /* leave the old network; ignored in the handler */
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    new_attempt_done(false, "Could not start connecting");
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi");
  }

  char buf[48];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';
  j_raw(&j, "{\"ok\":true,\"attempt\":");
  j_int(&j, (int)attempt);
  j_raw(&j, "}");
  return send_json(req, &j);
}

static esp_err_t volume_post(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len >= 64) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
  }
  char body[64];
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, body + got, req->content_len - got);
    if (n <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    got += (size_t)n;
  }
  body[got] = '\0';

  char level[8];
  if (form_get(body, "level", level, sizeof(level)) != 1 || level[0] == '\0') {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad level");
  }
  char *end = NULL;
  long v = strtol(level, &end, 10);
  if (*end != '\0' || v < 0 || v > 100) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad level");
  }
  audio_output_set_volume_percent((int)v);
  s_vol_touched = true;

  char buf[40];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';
  j_raw(&j, "{\"volume\":");
  j_int(&j, audio_output_get_volume_percent());
  j_raw(&j, "}");
  return send_json(req, &j);
}

static esp_err_t forget_post(httpd_req_t *req) {
  char buf[16] = "{\"ok\":true}";
  jbuf_t j = {.p = buf, .cap = sizeof(buf), .len = strlen(buf)};
  esp_err_t err = send_json(req, &j);

  ESP_LOGI(TAG, "Forgetting saved network");
  creds_erase();
  lock();
  s_ssid[0] = '\0';
  s_pass[0] = '\0';
  unlock();
  s_have_creds = false;
  s_trying_new = false;
  s_was_connected = false;
  s_connected = false;
  s_ip[0] = '\0';
  if (s_linger_timer) {
    xTimerStop(s_linger_timer, 0);
  }
  portal_open();
  return err;
}

/* Captive-portal probes (/generate_204, /hotspot-detect.html, ...) and any
 * other unknown URL go to the page.  iOS needs a body, not just a redirect. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  char loc[40];
  if (s_portal) {
    snprintf(loc, sizeof(loc), "http://%s/", s_ap_ip);
  } else {
    strlcpy(loc, "/", sizeof(loc));
  }
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "Redirect to the Wi-Fi setup page",
                  HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}


/* ── Saved settings: last station and last volume ────────────────────────── */
/* The station is saved whenever it has changed (from the page or the button).
 * The volume is saved after the page changed it.  Playback always starts on
 * boot; the volume starts low unless "start at last volume" is switched on. */

#define SET_NS "radio"

static int s_saved_station = -1;
static int s_saved_vol = -1;
static int s_boot_last_vol = -1; /* volume of the previous session (1-100) */
static bool s_vol_boot;          /* start at the last volume after a reboot */


/* ── Stations ────────────────────────────────────────────────────────────── */

static bool read_body(httpd_req_t *req, char *body, size_t size) {
  if (req->content_len == 0 || req->content_len >= size) {
    return false;
  }
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, body + got, req->content_len - got);
    if (n <= 0) {
      return false;
    }
    got += (size_t)n;
  }
  body[got] = '\0';
  return true;
}

static int current_station_index(const radio_player_stats_t *st) {
  for (size_t i = 0; i < g_station_count; i++) {
    if (st->station == g_stations[i].name ||
        (st->station && strcmp(st->station, g_stations[i].name) == 0)) {
      return (int)i;
    }
  }
  return 0;
}

static esp_err_t stations_get(httpd_req_t *req) {
  radio_player_stats_t st;
  radio_player_get_stats(&st);

  size_t cap = 700 + g_station_count * 140;
  char *p = malloc(cap);
  if (!p) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
  }
  p[0] = '\0';
  jbuf_t j = {.p = p, .cap = cap};
  j_raw(&j, "{\"current\":");
  j_int(&j, current_station_index(&st));
  j_raw(&j, ",\"streaming\":");
  j_raw(&j, st.streaming ? "true" : "false");
  j_raw(&j, ",\"paused\":");
  j_raw(&j, radio_player_is_paused() ? "true" : "false");
  j_raw(&j, ",\"lastvol\":");
  j_int(&j, s_boot_last_vol);
  j_raw(&j, ",\"volboot\":");
  j_raw(&j, s_vol_boot ? "true" : "false");
  j_raw(&j, ",\"title\":");
  j_str(&j, st.title);
  j_raw(&j, ",\"stations\":[");
  for (size_t i = 0; i < g_station_count; i++) {
    if (i) {
      j_raw(&j, ",");
    }
    j_str(&j, g_stations[i].name);
  }
  j_raw(&j, "]}");
  esp_err_t err = send_json(req, &j);
  free(p);
  return err;
}

static esp_err_t station_post(httpd_req_t *req) {
  char body[64];
  if (!read_body(req, body, sizeof(body))) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
  }
  char idx[8];
  if (form_get(body, "index", idx, sizeof(idx)) != 1 || idx[0] == '\0') {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
  }
  char *end = NULL;
  long v = strtol(idx, &end, 10);
  if (*end != '\0' || v < 0 || (size_t)v >= g_station_count) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
  }
  radio_player_select((size_t)v);

  char buf[40];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';
  j_raw(&j, "{\"current\":");
  j_int(&j, (int)v);
  j_raw(&j, "}");
  return send_json(req, &j);
}

static void settings_put(const char *key, uint32_t val, bool is_u8) {
  nvs_handle_t h;
  if (nvs_open(SET_NS, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  if (is_u8) {
    nvs_set_u8(h, key, (uint8_t)val);
  } else {
    nvs_set_u32(h, key, val);
  }
  nvs_commit(h);
  nvs_close(h);
}

static void settings_load(void) {
  nvs_handle_t h;
  if (nvs_open(SET_NS, NVS_READONLY, &h) != ESP_OK) {
    return;
  }
  uint32_t st = 0;
  uint8_t v = 0, f = 0;
  if (nvs_get_u32(h, "station", &st) == ESP_OK && st < g_station_count) {
    s_saved_station = (int)st;
  }
  if (nvs_get_u8(h, "volume", &v) == ESP_OK && v >= 1 && v <= 100) {
    s_boot_last_vol = v;
    s_saved_vol = v;
  }
  if (nvs_get_u8(h, "volboot", &f) == ESP_OK) {
    s_vol_boot = f != 0;
  }
  nvs_close(h);
}

/* Apply what was saved.  Called before the radio task starts. */
static void settings_apply(void) {
  if (s_saved_station > 0) {
    radio_player_select((size_t)s_saved_station);
  }
  if (s_vol_boot && s_boot_last_vol > 0) {
    ESP_LOGI(TAG, "Starting at the last volume, %d%%", s_boot_last_vol);
    audio_output_set_volume_percent(s_boot_last_vol);
  }
}

static void settings_task(void *arg) {
  (void)arg;
  int seen_st = -1, seen_vol = -1;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    radio_player_stats_t st;
    radio_player_get_stats(&st);
    int cur = current_station_index(&st);
    /* write only once the value has stayed the same for a whole period */
    if (cur == seen_st && cur != s_saved_station) {
      settings_put("station", (uint32_t)cur, false);
      s_saved_station = cur;
    }
    seen_st = cur;
    if (s_vol_touched) {
      int v = audio_output_get_volume_percent();
      if (v == seen_vol && v > 0 && v != s_saved_vol) {
        settings_put("volume", (uint32_t)v, true);
        s_saved_vol = v;
      }
      seen_vol = v;
    }
  }
}

static esp_err_t play_post(httpd_req_t *req) {
  char body[48], st[8];
  if (!read_body(req, body, sizeof(body)) ||
      form_get(body, "state", st, sizeof(st)) != 1) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
  }
  if (strcmp(st, "stop") == 0) {
    radio_player_set_paused(true);
  } else if (strcmp(st, "play") == 0) {
    radio_player_set_paused(false);
  } else {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad state");
  }
  char buf[32];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';
  j_raw(&j, "{\"paused\":");
  j_raw(&j, radio_player_is_paused() ? "true" : "false");
  j_raw(&j, "}");
  return send_json(req, &j);
}

static esp_err_t settings_post(httpd_req_t *req) {
  char body[48], v[4];
  if (!read_body(req, body, sizeof(body)) ||
      form_get(body, "volboot", v, sizeof(v)) != 1) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
  }
  s_vol_boot = v[0] == '1';
  settings_put("volboot", s_vol_boot ? 1 : 0, true);
  char buf[32];
  jbuf_t j = {.p = buf, .cap = sizeof(buf)};
  buf[0] = '\0';
  j_raw(&j, "{\"volboot\":");
  j_raw(&j, s_vol_boot ? "true" : "false");
  j_raw(&j, "}");
  return send_json(req, &j);
}

/* ── Serial console on the web page ──────────────────────────────────────── */
/* Everything the firmware logs is copied into a small ring buffer (and still
 * goes to the UART), and the page polls /api/log?since=N for new text. */

#define LOG_RING_BYTES 8192 /* power of two */
#define LOG_CHUNK      1536

static char s_log_ring[LOG_RING_BYTES];
static uint32_t s_log_total;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_prev;
static bool s_log_started;

static void log_push(const char *s, size_t n) {
  portENTER_CRITICAL(&s_log_mux);
  uint32_t t = s_log_total;
  for (size_t i = 0; i < n; i++) {
    s_log_ring[(t + i) & (LOG_RING_BYTES - 1)] = s[i];
  }
  s_log_total = t + (uint32_t)n;
  portEXIT_CRITICAL(&s_log_mux);
}

static int log_vprintf(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int ret = s_log_prev ? s_log_prev(fmt, ap) : vprintf(fmt, ap);
  char line[192];
  int n = vsnprintf(line, sizeof(line), fmt, ap2);
  va_end(ap2);
  if (n > 0) {
    if (n >= (int)sizeof(line)) {
      n = (int)sizeof(line) - 1;
    }
    int m = 0;
    for (int i = 0; i < n; i++) { /* drop ANSI colour codes */
      if (line[i] == 0x1b) {
        while (i < n && line[i] != 'm') {
          i++;
        }
        continue;
      }
      line[m++] = line[i];
    }
    log_push(line, (size_t)m);
  }
  return ret;
}

static void log_capture_start(void) {
  if (s_log_started) {
    return;
  }
  s_log_started = true;
  s_log_prev = esp_log_set_vprintf(log_vprintf);
}

static esp_err_t log_get(httpd_req_t *req) {
  uint32_t since = 0;
  char q[40], v[16];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
    since = (uint32_t)strtoul(v, NULL, 10);
  }
  char *buf = malloc(LOG_CHUNK);
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
  }

  bool reset = false;
  uint32_t start, next;
  size_t len;
  portENTER_CRITICAL(&s_log_mux);
  uint32_t total = s_log_total;
  uint32_t oldest = total > LOG_RING_BYTES ? total - LOG_RING_BYTES : 0;
  start = since;
  if (since > total) { /* the device restarted since the page last asked */
    reset = true;
    start = oldest;
  } else if (start < oldest) {
    start = oldest;
  }
  len = total - start;
  if (len > LOG_CHUNK) {
    len = LOG_CHUNK;
  }
  for (size_t i = 0; i < len; i++) {
    buf[i] = s_log_ring[(start + i) & (LOG_RING_BYTES - 1)];
  }
  next = start + (uint32_t)len;
  portEXIT_CRITICAL(&s_log_mux);

  char nxt[16];
  snprintf(nxt, sizeof(nxt), "%lu", (unsigned long)next);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Next", nxt);
  if (reset) {
    httpd_resp_set_hdr(req, "X-Reset", "1");
  }
  esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
  free(buf);
  return err;
}

/* ── mDNS: answers "webradio.local" ──────────────────────────────────────── */
/* A small responder for one name.  Apple devices ask for A and AAAA together;
 * answering AAAA with an NSEC ("no IPv6 address") lets them stop waiting, which
 * is what makes lookups fast and reliable.  Replies go to the multicast group
 * and, for "unicast please" (QU) questions, also straight to the asker. */

#define MDNS_PORT 5353
#define MDNS_NAME "webradio.local"
#define MDNS_ANNOUNCE_MS 45000

static volatile int s_mdns_announce;
static bool s_mdns_started;

static const uint8_t k_mdns_name[] = {8, 'w', 'e', 'b', 'r', 'a', 'd', 'i', 'o',
                                      5, 'l', 'o', 'c', 'a', 'l', 0};

/* Read a (non-compressed) name starting at pos into out as "a.b.c".
 * Returns the position after the name, or 0 if it is not usable. */
static size_t mdns_read_name(const uint8_t *pkt, size_t len, size_t pos,
                             char *out, size_t out_size) {
  size_t o = 0;
  while (pos < len) {
    uint8_t l = pkt[pos++];
    if (l == 0) {
      out[o] = '\0';
      return pos;
    }
    if ((l & 0xC0) || pos + l > len || o + l + 2 > out_size) {
      return 0;
    }
    if (o) {
      out[o++] = '.';
    }
    for (uint8_t i = 0; i < l; i++) {
      out[o++] = (char)tolower(pkt[pos++]);
    }
  }
  return 0;
}

/* Build a response with an A record and/or an NSEC record for our name.
 * question != NULL builds a legacy unicast reply that repeats the question. */
static size_t mdns_build(uint8_t *out, uint16_t id, const uint8_t *question,
                         size_t qlen, uint32_t ip_be, bool want_a,
                         bool want_nsec) {
  size_t n = 0;
  out[n++] = (uint8_t)(id >> 8);
  out[n++] = (uint8_t)id;
  out[n++] = 0x84; /* response, authoritative */
  out[n++] = 0x00;
  out[n++] = 0;
  out[n++] = question ? 1 : 0;
  out[n++] = 0;
  out[n++] = (uint8_t)((want_a ? 1 : 0) + (want_nsec ? 1 : 0));
  out[n++] = 0;
  out[n++] = 0;
  out[n++] = 0;
  out[n++] = 0;
  if (question) {
    memcpy(out + n, question, qlen);
    n += qlen;
  }
  const uint8_t cls = question ? 0x00 : 0x80; /* cache-flush bit (multicast) */
  if (want_a) {
    memcpy(out + n, k_mdns_name, sizeof(k_mdns_name));
    n += sizeof(k_mdns_name);
    const uint8_t rr[] = {0, 1, cls, 1, 0, 0, 0, 120, 0, 4};
    memcpy(out + n, rr, sizeof(rr));
    n += sizeof(rr);
    memcpy(out + n, &ip_be, 4);
    n += 4;
  }
  if (want_nsec) {
    memcpy(out + n, k_mdns_name, sizeof(k_mdns_name));
    n += sizeof(k_mdns_name);
    const uint8_t rr[] = {0, 47, cls, 1, 0, 0, 0, 120, 0, 19};
    memcpy(out + n, rr, sizeof(rr));
    n += sizeof(rr);
    memcpy(out + n, k_mdns_name, sizeof(k_mdns_name)); /* next name */
    n += sizeof(k_mdns_name);
    const uint8_t bitmap[] = {0, 1, 0x40}; /* only type A (1) exists */
    memcpy(out + n, bitmap, sizeof(bitmap));
    n += sizeof(bitmap);
  }
  return n;
}

static void mdns_join(int sock, esp_netif_ip_info_t *ip, bool have_ip) {
  struct ip_mreq mreq = {
      .imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
      .imr_interface.s_addr = htonl(INADDR_ANY),
  };
  /* Leave and join again so the router sees a fresh membership report. */
  setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ESP_LOGW(TAG, "mDNS: could not join the multicast group");
  }
  if (have_ip) {
    struct in_addr ifa = {.s_addr = ip->ip.addr};
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
  }
}

static void mdns_task(void *arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGW(TAG, "mDNS: no socket");
    s_mdns_started = false;
    vTaskDelete(NULL);
    return;
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in any = {
      .sin_family = AF_INET,
      .sin_port = htons(MDNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&any, sizeof(any)) < 0) {
    ESP_LOGW(TAG, "mDNS: bind failed");
    close(sock);
    s_mdns_started = false;
    vTaskDelete(NULL);
    return;
  }
  uint8_t ttl = 255;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in group = {
      .sin_family = AF_INET,
      .sin_port = htons(MDNS_PORT),
      .sin_addr.s_addr = inet_addr("224.0.0.251"),
  };
  ESP_LOGI(TAG, "mDNS: answering http://" MDNS_NAME "/");

  uint8_t rx[512];
  uint8_t tx[192];
  TickType_t last_announce = 0;
  uint32_t last_ip = 0;

  while (true) {
    esp_netif_ip_info_t ip = {0};
    bool have_ip = s_sta_netif &&
                   esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK &&
                   ip.ip.addr != 0;

    if (have_ip && ip.ip.addr != last_ip) { /* new address: join and announce */
      last_ip = ip.ip.addr;
      s_mdns_announce = 3; /* joins the group again and announces */
    }
    TickType_t now = xTaskGetTickCount();
    if (have_ip && s_mdns_announce == 0 &&
        (now - last_announce) > pdMS_TO_TICKS(MDNS_ANNOUNCE_MS)) {
      s_mdns_announce = 1; /* refresh other devices' caches now and then */
    }
    if (s_mdns_announce > 0) {
      if (s_mdns_announce == 3) {
        mdns_join(sock, &ip, have_ip);
      }
      s_mdns_announce--;
      if (have_ip) {
        size_t n = mdns_build(tx, 0, NULL, 0, ip.ip.addr, true, true);
        sendto(sock, tx, n, 0, (struct sockaddr *)&group, sizeof(group));
        last_announce = now;
      }
    }

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
    if (len < 12 || !have_ip) {
      continue;
    }
    if (rx[2] & 0x80) {
      continue; /* a response, not a query */
    }
    int qd = (rx[4] << 8) | rx[5];
    size_t pos = 12;
    bool want_a = false, want_nsec = false, qu = false;
    size_t first_q = 0, first_qlen = 0;
    for (int i = 0; i < qd && pos < (size_t)len; i++) {
      size_t qstart = pos;
      char name[64];
      pos = mdns_read_name(rx, (size_t)len, pos, name, sizeof(name));
      if (pos == 0 || pos + 4 > (size_t)len) {
        break;
      }
      int type = (rx[pos] << 8) | rx[pos + 1];
      int cls = (rx[pos + 2] << 8) | rx[pos + 3];
      pos += 4;
      if (strcmp(name, MDNS_NAME) != 0) {
        continue;
      }
      bool hit = true;
      if (type == 1 || type == 255) {
        want_a = true;
      } else if (type == 28) {
        want_nsec = true; /* we have no IPv6 address */
      } else {
        hit = false;
      }
      if (hit) {
        if (cls & 0x8000) {
          qu = true;
        }
        if (!first_qlen) {
          first_q = qstart;
          first_qlen = pos - qstart;
        }
      }
    }
    if (!want_a && !want_nsec) {
      continue;
    }
    if (ntohs(from.sin_port) != MDNS_PORT) {
      /* plain unicast DNS query (e.g. nslookup): reply to the sender only */
      uint16_t id = (uint16_t)((rx[0] << 8) | rx[1]);
      size_t n = mdns_build(tx, id, rx + first_q, first_qlen, ip.ip.addr,
                            want_a, want_nsec);
      sendto(sock, tx, n, 0, (struct sockaddr *)&from, sizeof(from));
      continue;
    }
    size_t n = mdns_build(tx, 0, NULL, 0, ip.ip.addr, want_a, want_nsec);
    sendto(sock, tx, n, 0, (struct sockaddr *)&group, sizeof(group));
    if (qu) {
      sendto(sock, tx, n, 0, (struct sockaddr *)&from, sizeof(from));
    }
  }
}

static void mdns_start_once(void) {
  s_mdns_announce = 3;
  if (s_mdns_started) {
    return;
  }
  s_mdns_started = true;
  if (xTaskCreate(mdns_task, "mdns", 4096, NULL, 4, NULL) != pdPASS) {
    s_mdns_started = false;
  }
}

static esp_err_t web_start(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 7;
  cfg.max_uri_handlers = 16;
  cfg.lru_purge_enable = true;

  /* Redirecting probe traffic makes the server log lots of harmless errors. */
  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

  esp_err_t err = httpd_start(&s_httpd, &cfg);
  if (err != ESP_OK) {
    return err;
  }
  static const httpd_uri_t uris[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_get},
      {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_get},
      {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
      {.uri = "/api/connect", .method = HTTP_POST, .handler = connect_post},
      {.uri = "/api/forget", .method = HTTP_POST, .handler = forget_post},
      {.uri = "/api/volume", .method = HTTP_POST, .handler = volume_post},
      {.uri = "/api/stations", .method = HTTP_GET, .handler = stations_get},
      {.uri = "/api/station", .method = HTTP_POST, .handler = station_post},
      {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
      {.uri = "/api/play", .method = HTTP_POST, .handler = play_post},
      {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_httpd, &uris[i]);
  }
  httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found);
  return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_start(void) {
  log_capture_start();
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    return err;
  }

  s_lock = xSemaphoreCreateMutex();
  s_linger_timer = xTimerCreate("wifi_linger", pdMS_TO_TICKS(PORTAL_LINGER_MS),
                                pdFALSE, NULL, linger_cb);
  s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(SAVED_RETRY_MS),
                               pdTRUE, NULL, retry_cb);
  if (!s_lock || !s_linger_timer || !s_retry_timer) {
    return ESP_ERR_NO_MEM;
  }

  creds_load();
  settings_load();

  err = esp_netif_init();
  if (err != ESP_OK) {
    return err;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }
  s_sta_netif = esp_netif_create_default_wifi_sta();
  s_ap_netif = esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(s_sta_netif, HOSTNAME);

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) {
    return err;
  }
  esp_wifi_set_storage(WIFI_STORAGE_RAM); /* we keep credentials in our NVS */
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

  esp_wifi_set_mode(WIFI_MODE_STA);
  if (s_have_creds) {
    sta_apply_config(s_ssid, s_pass);
    s_sta_wanted = true;
    s_state = WIFI_MANAGER_CONNECTING;
    ESP_LOGI(TAG, "Connecting to saved network \"%s\"", s_ssid);
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    return err;
  }
  esp_wifi_set_ps(WIFI_PS_NONE); /* modem sleep adds jitter to the stream */

  err = web_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Web server failed: %s", esp_err_to_name(err));
  }
  settings_apply();
  xTaskCreate(settings_task, "settings", 4096, NULL, 2, NULL);

  if (!s_have_creds) {
    ESP_LOGI(TAG, "No saved network");
    portal_open();
  }
  return ESP_OK;
}

bool wifi_manager_is_connected(void) {
  return s_connected;
}

bool wifi_manager_portal_active(void) {
  return s_portal;
}

wifi_manager_state_t wifi_manager_get_state(void) {
  return s_state;
}
