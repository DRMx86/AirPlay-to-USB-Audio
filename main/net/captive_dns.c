#include "captive_dns.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdbool.h>
#include <string.h>

#define TAG "captive_dns"

#define DNS_PORT 53
#define DNS_MAX  512
#define DNS_TTL  60

size_t captive_dns_build_reply(uint8_t *pkt, size_t len, size_t cap,
                               uint32_t ip_be) {
  if (len < 12 || cap < len) {
    return 0;
  }
  uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
  if (flags & 0x8000) {
    return 0; /* already a response */
  }
  if (((flags >> 11) & 0xF) != 0) {
    return 0; /* not a standard query */
  }
  if (((pkt[4] << 8) | pkt[5]) != 1) {
    return 0; /* expect exactly one question */
  }

  /* Walk the question name: length-prefixed labels ending in a zero byte. */
  size_t i = 12;
  while (i < len && pkt[i] != 0) {
    if (pkt[i] & 0xC0) {
      return 0; /* compression is not allowed in a question */
    }
    i += (size_t)pkt[i] + 1;
  }
  if (i >= len) {
    return 0;
  }
  i++; /* root label */
  if (i + 4 > len) {
    return 0;
  }
  uint16_t qtype = (uint16_t)((pkt[i] << 8) | pkt[i + 1]);
  uint16_t qclass = (uint16_t)((pkt[i + 2] << 8) | pkt[i + 3]);
  size_t out = i + 4; /* keep header + question, drop extra records */

  pkt[2] = (uint8_t)(0x80 | (pkt[2] & 0x01)); /* QR=1, keep RD */
  pkt[3] = 0x80;                              /* RA=1, RCODE=0 */
  memset(pkt + 6, 0, 6); /* no answer, authority or additional records */

  if (qtype == 1 && qclass == 1) { /* A, IN */
    if (out + 16 > cap) {
      return 0;
    }
    uint8_t *a = pkt + out;
    a[0] = 0xC0; /* name: pointer to the question at offset 12 */
    a[1] = 0x0C;
    a[2] = 0x00; /* type A */
    a[3] = 0x01;
    a[4] = 0x00; /* class IN */
    a[5] = 0x01;
    a[6] = 0x00; /* TTL */
    a[7] = 0x00;
    a[8] = 0x00;
    a[9] = DNS_TTL;
    a[10] = 0x00; /* RDLENGTH 4 */
    a[11] = 0x04;
    memcpy(a + 12, &ip_be, 4);
    pkt[7] = 1; /* ANCOUNT */
    out += 16;
  }
  return out;
}

static volatile bool s_run;
static TaskHandle_t s_task;
static uint32_t s_ip;

static void dns_task(void *arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket() failed");
    goto exit;
  }
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed");
    close(sock);
    goto exit;
  }
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  uint8_t buf[DNS_MAX];
  while (s_run) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from,
                     &from_len);
    if (n <= 0) {
      continue; /* timeout: re-check s_run */
    }
    size_t reply = captive_dns_build_reply(buf, (size_t)n, sizeof(buf), s_ip);
    if (reply > 0) {
      sendto(sock, buf, reply, 0, (struct sockaddr *)&from, from_len);
    }
  }
  close(sock);

exit:
  s_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ip_be) {
  /* A previous stop may still be winding down. */
  for (int i = 0; i < 15 && s_task != NULL && !s_run; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  s_ip = ip_be;
  if (s_task != NULL) {
    return ESP_OK; /* still running */
  }
  s_run = true;
  if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, &s_task) != pdPASS) {
    s_run = false;
    s_task = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void captive_dns_stop(void) {
  s_run = false;
}
