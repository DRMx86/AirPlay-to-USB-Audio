/**
 * USB Audio Class (UAC) output — ESP32 as a USB audio source.
 *
 * The ESP32 enumerates as a USB microphone (audio input device) on the
 * connected host.  PCM captured from the I2S input is fed to the host via the
 * UAC input callback, which lets a PC record it (Audacity, OBS, ...) or lets a
 * USB-input amplifier / DAC play it.
 *
 * Uses the espressif/usb_device_uac managed component, which handles all USB
 * descriptor, endpoint and class-level protocol details.
 *
 * Data path:
 *   capture task --audio_output_write()--> ring buffer --usb_input_cb()--> host
 *
 * The USB side is the clock master from the host's point of view (1 ms SOF),
 * while the I2S side runs from the source's clock.  The two are never exactly
 * equal, so the ring buffer slowly fills or drains.  Overflow drops the excess
 * frames; underflow sends silence and waits for the buffer to refill
 * (CONFIG_USB_AUDIO_PRIME_MS) before streaming resumes.
 */

#include "audio_output.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "usb_device_uac.h"
#include <string.h>

#define TAG         "audio_usb"
#define OUTPUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_BYTES 4 /* stereo, 16-bit */

#if CONFIG_OUTPUT_SAMPLE_RATE_HZ != CONFIG_UAC_SAMPLE_RATE
#error "CONFIG_OUTPUT_SAMPLE_RATE_HZ must match CONFIG_UAC_SAMPLE_RATE"
#endif

#define BYTES_PER_MS  ((size_t)(OUTPUT_RATE / 1000) * FRAME_BYTES)
#define RINGBUF_BYTES (BYTES_PER_MS * CONFIG_USB_AUDIO_BUFFER_MS)
#define PRIME_BYTES   (BYTES_PER_MS * CONFIG_USB_AUDIO_PRIME_MS)

#if CONFIG_USB_AUDIO_PRIME_MS >= CONFIG_USB_AUDIO_BUFFER_MS
#error \
    "CONFIG_USB_AUDIO_PRIME_MS must be smaller than CONFIG_USB_AUDIO_BUFFER_MS"
#endif

static RingbufHandle_t s_ringbuf;
static volatile bool s_streaming;
static volatile uint32_t s_overruns;
static volatile uint32_t s_underruns;

/* ── USB UAC input callback (pull model) ──────────────────────────────
 * Called by the UAC stack when the host requests audio data.  Always fills
 * the whole request: real audio when streaming, silence otherwise.        */

static esp_err_t usb_input_cb(uint8_t *buf, size_t len, size_t *bytes_read,
                              void *cb_ctx) {
  (void)cb_ctx;
  RingbufHandle_t rb = s_ringbuf;
  size_t got = 0;

  if (rb) {
    if (!s_streaming) {
      size_t filled = RINGBUF_BYTES - xRingbufferGetCurFreeSize(rb);
      if (filled >= PRIME_BYTES) {
        s_streaming = true;
      }
    }

    if (s_streaming) {
      /* A byte buffer hands back at most two chunks when the data wraps. */
      while (got < len) {
        size_t item_size = 0;
        void *data = xRingbufferReceiveUpTo(rb, &item_size, 0, len - got);
        if (!data) {
          break;
        }
        memcpy(buf + got, data, item_size);
        vRingbufferReturnItem(rb, data);
        got += item_size;
      }
      if (got < len) {
        /* Ran dry: go back to priming so we resume with some headroom. */
        s_streaming = false;
        s_underruns++;
      }
    }
  }

  if (got < len) {
    memset(buf + got, 0, len - got);
  }
  *bytes_read = len;
  return ESP_OK;
}

/* ── Public API ────────────────────────────────────────────────────── */

esp_err_t audio_output_init(void) {
  ESP_LOGI(TAG,
           "Initialising USB UAC output (%d Hz, buffer %d ms, prime %d ms)",
           OUTPUT_RATE, CONFIG_USB_AUDIO_BUFFER_MS, CONFIG_USB_AUDIO_PRIME_MS);

  s_ringbuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
  if (!s_ringbuf) {
    ESP_LOGE(TAG, "Failed to create ring buffer");
    return ESP_ERR_NO_MEM;
  }

  uac_device_config_t uac_cfg = {
      .skip_tinyusb_init = false,
      .output_cb = NULL,        /* not a speaker — no host-to-device audio */
      .input_cb = usb_input_cb, /* device-to-host: I2S audio out */
      .set_mute_cb = NULL,
      .set_volume_cb = NULL,
      .cb_ctx = NULL,
  };

  esp_err_t err = uac_device_init(&uac_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UAC device init failed: %s", esp_err_to_name(err));
    RingbufHandle_t rb = s_ringbuf;
    s_ringbuf = NULL;
    vRingbufferDelete(rb);
    return err;
  }

  ESP_LOGI(TAG, "USB UAC output ready");
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
