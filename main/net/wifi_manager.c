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
#include "nvs.h"
#include "nvs_flash.h"
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
    return httpd_resp_send_err(req, HTTPD_409_CONFLICT, "busy");
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

static esp_err_t web_start(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 7;
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
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_httpd, &uris[i]);
  }
  httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found);
  return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_start(void) {
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
