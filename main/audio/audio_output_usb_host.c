/**
 * USB Audio Class (UAC) HOST output -- ESP32-S3 drives a USB DAC dongle.
 *
 * Same API as audio_output.h (i2s-to-USB-Audio branch), so audio_input_i2s.c
 * and main.c do not change.
 *
 * Data path:
 *   capture task --audio_output_write()--> ring buffer --writer task--> dongle
 *
 * Needs:  idf_component.yml:  espressif/usb_host_uac
 * Wiring: the S3's native USB OTG port (GPIO19 D-, GPIO20 D+), with 5 V on
 *         VBUS.  The UART/USB-serial port cannot be used for this.
 *
 * NOTE: written against the espressif/usb_host_uac API as I remember it.
 * If a struct field or enum name differs in your installed version, the
 * compiler will point at it; the structure stays the same.
 */

#include "audio_output.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "usb/uac_host.h"
#include "usb/usb_host.h"
#include <string.h>

#define TAG         "audio_usbh"
#define OUTPUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_BYTES 4 /* stereo, 16-bit */

#define BYTES_PER_MS  ((size_t)(OUTPUT_RATE / 1000) * FRAME_BYTES)
#define RINGBUF_BYTES (BYTES_PER_MS * CONFIG_USB_AUDIO_BUFFER_MS)
#define PRIME_BYTES   (BYTES_PER_MS * CONFIG_USB_AUDIO_PRIME_MS)
#define CHUNK_MS      10
#define CHUNK_BYTES   (BYTES_PER_MS * CHUNK_MS)

#if CONFIG_USB_AUDIO_PRIME_MS >= CONFIG_USB_AUDIO_BUFFER_MS
#error "CONFIG_USB_AUDIO_PRIME_MS must be smaller than CONFIG_USB_AUDIO_BUFFER_MS"
#endif

typedef struct {
  uint8_t addr;
  uint8_t iface;
  bool connected; /* true = connect, false = disconnect */
} dev_event_t;

static RingbufHandle_t s_ringbuf;
static QueueHandle_t s_evq;
static volatile bool s_streaming;
static volatile uint32_t s_overruns;
static volatile uint32_t s_underruns;
static volatile uac_host_device_handle_t s_dev;

/* ── USB host library task ─────────────────────────────────────────── */

static void usb_lib_task(void *arg) {
  (void)arg;
  while (true) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

/* ── UAC callbacks (just forward to the writer task) ───────────────── */

static void uac_device_cb(uac_host_device_handle_t h,
                          const uac_host_device_event_t event, void *arg) {
  (void)arg;
  if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
    dev_event_t ev = {.connected = false};
    s_dev = NULL;
    uac_host_device_close(h);
    xQueueSend(s_evq, &ev, 0);
  } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
    ESP_LOGW(TAG, "USB transfer error");
  }
}

static void uac_driver_cb(uint8_t addr, uint8_t iface,
                          const uac_host_driver_event_t event, void *arg) {
  (void)arg;
  if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) { /* a speaker/DAC */
    dev_event_t ev = {.addr = addr, .iface = iface, .connected = true};
    xQueueSend(s_evq, &ev, 0);
  }
}

/* ── Open and start the dongle ─────────────────────────────────────── */

static esp_err_t start_dongle(uint8_t addr, uint8_t iface) {
  uac_host_device_config_t dcfg = {
      .addr = addr,
      .iface_num = iface,
      .buffer_size = OUTPUT_RATE / 1000 * FRAME_BYTES * 50, /* 50 ms */
      .buffer_threshold = OUTPUT_RATE / 1000 * FRAME_BYTES * 20,
      .callback = uac_device_cb,
      .callback_arg = NULL,
  };
  uac_host_device_handle_t h = NULL;
  ESP_RETURN_ON_ERROR(uac_host_device_open(&dcfg, &h), TAG, "open failed");

  uac_host_stream_config_t scfg = {
      .channels = 2,
      .bit_resolution = 16,
      .sample_freq = OUTPUT_RATE,
  };
  esp_err_t err = uac_host_device_start(h, &scfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG,
             "Dongle rejected %d Hz / 16-bit / stereo (%s). Try 44100 or "
             "48000 in CONFIG_OUTPUT_SAMPLE_RATE_HZ.",
             OUTPUT_RATE, esp_err_to_name(err));
    uac_host_device_close(h);
    return err;
  }
  s_dev = h;
  s_streaming = false; /* re-prime on every new connection */
  ESP_LOGI(TAG, "USB DAC started: %d Hz, 16-bit, stereo", OUTPUT_RATE);
  return ESP_OK;
}

/* ── Writer task: ring buffer -> dongle ────────────────────────────── */

static void writer_task(void *arg) {
  (void)arg;
  uint8_t *chunk = malloc(CHUNK_BYTES);
  if (!chunk) {
    ESP_LOGE(TAG, "no memory for chunk");
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    /* Handle connect / disconnect events. */
    dev_event_t ev;
    while (xQueueReceive(s_evq, &ev, s_dev ? 0 : pdMS_TO_TICKS(100))) {
      if (ev.connected) {
        if (!s_dev) {
          start_dongle(ev.addr, ev.iface);
        }
      } else {
        ESP_LOGW(TAG, "USB DAC disconnected");
        s_streaming = false;
      }
    }

    uac_host_device_handle_t h = s_dev;
    if (!h) {
      continue;
    }

    /* Prime before streaming so we start with headroom. */
    if (!s_streaming) {
      size_t filled = RINGBUF_BYTES - xRingbufferGetCurFreeSize(s_ringbuf);
      if (filled >= PRIME_BYTES) {
        s_streaming = true;
      }
    }

    size_t got = 0;
    if (s_streaming) {
      while (got < CHUNK_BYTES) {
        size_t item_size = 0;
        void *data = xRingbufferReceiveUpTo(s_ringbuf, &item_size, 0,
                                            CHUNK_BYTES - got);
        if (!data) {
          break;
        }
        memcpy(chunk + got, data, item_size);
        vRingbufferReturnItem(s_ringbuf, data);
        got += item_size;
      }
      if (got < CHUNK_BYTES) {
        s_streaming = false; /* ran dry: back to priming */
        s_underruns++;
      }
    }
    if (got < CHUNK_BYTES) {
      memset(chunk + got, 0, CHUNK_BYTES - got); /* silence */
    }

    /* Blocks until the dongle's buffer has room: this paces the loop. */
    esp_err_t err = uac_host_device_write(h, chunk, CHUNK_BYTES, 100);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

/* ── Public API (same as the device-mode file) ─────────────────────── */

esp_err_t audio_output_init(void) {
  ESP_LOGI(TAG, "USB host UAC output (%d Hz, buffer %d ms, prime %d ms)",
           OUTPUT_RATE, CONFIG_USB_AUDIO_BUFFER_MS, CONFIG_USB_AUDIO_PRIME_MS);

  s_ringbuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
  s_evq = xQueueCreate(4, sizeof(dev_event_t));
  ESP_RETURN_ON_FALSE(s_ringbuf && s_evq, ESP_ERR_NO_MEM, TAG, "no memory");

  usb_host_config_t host_cfg = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
  xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL, 0);

  uac_host_driver_config_t drv_cfg = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = uac_driver_cb,
      .callback_arg = NULL,
  };
  ESP_RETURN_ON_ERROR(uac_host_install(&drv_cfg), TAG, "uac_host_install");

  xTaskCreatePinnedToCore(writer_task, "usb_writer", 4096, NULL, 6, NULL, 0);
  ESP_LOGI(TAG, "Waiting for a USB DAC...");
  return ESP_OK;
}

size_t audio_output_write(const int16_t *pcm, size_t frames) {
  RingbufHandle_t rb = s_ringbuf;
  if (!rb || !pcm || frames == 0) {
    return 0;
  }
  size_t bytes = frames * FRAME_BYTES;
  size_t space = xRingbufferGetCurFreeSize(rb) & ~(size_t)(FRAME_BYTES - 1);
  size_t n = bytes < space ? bytes : space;
  if (n < bytes) {
    s_overruns++;
  }
  if (n > 0 && xRingbufferSend(rb, pcm, n, 0) != pdTRUE) {
    s_overruns++;
    n = 0;
  }
  return n / FRAME_BYTES;
}

void audio_output_flush(void) {
  RingbufHandle_t rb = s_ringbuf;
  if (!rb) {
    return;
  }
  s_streaming = false;
  size_t item_size;
  void *item;
  while ((item = xRingbufferReceive(rb, &item_size, 0)) != NULL) {
    vRingbufferReturnItem(rb, item);
  }
}

void audio_output_get_stats(audio_output_stats_t *stats) {
  if (!stats) {
    return;
  }
  RingbufHandle_t rb = s_ringbuf;
  stats->streaming = s_streaming;
  stats->capacity_bytes = RINGBUF_BYTES;
  stats->fill_bytes =
      rb ? (uint32_t)(RINGBUF_BYTES - xRingbufferGetCurFreeSize(rb)) : 0;
  stats->overruns = s_overruns;
  stats->underruns = s_underruns;
}
