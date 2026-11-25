/**
 * I2S capture -> USB audio bridge input side.
 *
 * Reads stereo I2S frames from the configured pins, converts them to 16-bit
 * PCM and hands them to the USB output.  Slave mode (the source owns BCK/WS)
 * is the default; master mode makes the ESP32 generate BCK/WS/MCLK for
 * converters such as the PCM1808.
 */

#include "audio_input_i2s.h"

#include "audio_output.h"
#include "audio_resample.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

#define TAG "audio_in"

#define IN_RATE   CONFIG_I2S_IN_RATE_HZ
#define OUT_RATE  CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define SLOT_BITS CONFIG_I2S_IN_SLOT_BITS

/* 5 ms of audio per read at 48 kHz.  Also the DMA frame count, so one read
 * returns roughly one DMA buffer. */
#define CAPTURE_FRAMES 240
#define IN_FRAME_BYTES (2 * (SLOT_BITS / 8))

/* Worst-case frames after resampling one capture block. */
#define MAX_OUT_FRAMES \
  ((size_t)((CAPTURE_FRAMES + 2) * ((double)OUT_RATE / IN_RATE) + 16))

#define READ_TIMEOUT_MS 100
/* The source is considered gone after this many consecutive empty reads. */
#define INACTIVE_AFTER_TIMEOUTS 5

#if CONFIG_FREERTOS_UNICORE
#define CAPTURE_CORE 0
#else
#define CAPTURE_CORE 1
#endif

#if CONFIG_I2S_IN_ROLE_MASTER
#define I2S_IN_ROLE I2S_ROLE_MASTER
#else
#define I2S_IN_ROLE I2S_ROLE_SLAVE
#endif

#if SLOT_BITS == 16
#define I2S_IN_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#else
#define I2S_IN_WIDTH I2S_DATA_BIT_WIDTH_32BIT
#endif

#if CONFIG_I2S_IN_FORMAT_MSB
#define I2S_IN_SLOT_CFG(w, m) I2S_STD_MSB_SLOT_DEFAULT_CONFIG(w, m)
#define I2S_IN_FORMAT_STR     "MSB"
#else
#define I2S_IN_SLOT_CFG(w, m) I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(w, m)
#define I2S_IN_FORMAT_STR     "Philips"
#endif

#if CONFIG_I2S_IN_MCLK_IO >= 0
#define I2S_IN_MCLK_PIN CONFIG_I2S_IN_MCLK_IO
#else
#define I2S_IN_MCLK_PIN I2S_GPIO_UNUSED
#endif

static i2s_chan_handle_t s_rx;
static volatile bool s_active;
static volatile uint32_t s_frames;
static volatile uint32_t s_timeouts;

/* Convert raw I2S slots to interleaved int16.  32-bit slots carry up to 24
 * significant bits left-aligned, so keeping the top 16 bits is a plain
 * truncation to 16-bit audio. */
static void convert_to_s16(const uint8_t *raw, int16_t *out, size_t samples) {
#if SLOT_BITS == 32
  const int32_t *src = (const int32_t *)raw;
  for (size_t i = 0; i < samples; i++) {
    out[i] = (int16_t)(src[i] >> 16);
  }
#else
  const int16_t *src = (const int16_t *)raw;
  for (size_t i = 0; i < samples; i++) {
    out[i] = src[i];
  }
#endif
}

static void capture_task(void *arg) {
  (void)arg;
  uint8_t *raw = malloc((size_t)CAPTURE_FRAMES * IN_FRAME_BYTES);
  int16_t *pcm = malloc((size_t)CAPTURE_FRAMES * 2 * sizeof(int16_t));
  int16_t *resampled = NULL;
  if (IN_RATE != OUT_RATE) {
    resampled = malloc(MAX_OUT_FRAMES * 2 * sizeof(int16_t));
  }
  if (!raw || !pcm || (IN_RATE != OUT_RATE && !resampled)) {
    ESP_LOGE(TAG, "Failed to allocate capture buffers");
    free(raw);
    free(pcm);
    free(resampled);
    vTaskDelete(NULL);
    return;
  }

  uint32_t consecutive_timeouts = 0;

  while (true) {
    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(s_rx, raw, (size_t)CAPTURE_FRAMES * IN_FRAME_BYTES,
                         &bytes_read, READ_TIMEOUT_MS);

    size_t frames = bytes_read / IN_FRAME_BYTES;
    if (frames == 0) {
      if (err == ESP_ERR_TIMEOUT) {
        s_timeouts++;
        if (++consecutive_timeouts == INACTIVE_AFTER_TIMEOUTS && s_active) {
          s_active = false;
          ESP_LOGW(TAG, "No I2S data (is the source running and clocking?)");
        }
      } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      continue;
    }

    if (!s_active) {
      s_active = true;
      ESP_LOGI(TAG, "I2S data flowing");
    }
    consecutive_timeouts = 0;

    convert_to_s16(raw, pcm, frames * 2);
    s_frames += (uint32_t)frames;

    const int16_t *out = pcm;
    size_t out_frames = frames;
    if (resampled && audio_resample_is_active()) {
      out_frames =
          audio_resample_process(pcm, frames, resampled, MAX_OUT_FRAMES);
      out = resampled;
    }
    audio_output_write(out, out_frames);
  }
}

esp_err_t audio_input_init(void) {
  ESP_LOGI(
      TAG,
      "I2S input: %s, %d Hz, %d-bit slots, %s, BCK=%d WS=%d DIN=%d MCLK=%d",
      I2S_IN_ROLE == I2S_ROLE_MASTER ? "master" : "slave", IN_RATE, SLOT_BITS,
      I2S_IN_FORMAT_STR, CONFIG_I2S_IN_BCK_IO, CONFIG_I2S_IN_WS_IO,
      CONFIG_I2S_IN_DIN_IO, CONFIG_I2S_IN_MCLK_IO);

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_IN_ROLE);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = CAPTURE_FRAMES;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG,
                      "i2s_new_channel failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(IN_RATE),
      .slot_cfg = I2S_IN_SLOT_CFG(I2S_IN_WIDTH, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_IN_MCLK_PIN,
              .bclk = CONFIG_I2S_IN_BCK_IO,
              .ws = CONFIG_I2S_IN_WS_IO,
              .dout = I2S_GPIO_UNUSED,
              .din = CONFIG_I2S_IN_DIN_IO,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  esp_err_t err = i2s_channel_init_std_mode(s_rx, &std_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return err;
  }

  if (IN_RATE != OUT_RATE && !audio_resample_init(IN_RATE, OUT_RATE, 2)) {
    ESP_LOGE(TAG, "Failed to initialise resampler (%d -> %d Hz)", IN_RATE,
             OUT_RATE);
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t audio_input_start(void) {
  if (!s_rx) {
    return ESP_ERR_INVALID_STATE;
  }
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG,
                      "i2s_channel_enable failed");

  BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "i2s_capture", 6144,
                                          NULL, 7, NULL, CAPTURE_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create capture task");
    i2s_channel_disable(s_rx);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void audio_input_get_stats(audio_input_stats_t *stats) {
  if (!stats) {
    return;
  }
  stats->active = s_active;
  stats->frames_captured = s_frames;
  stats->read_timeouts = s_timeouts;
}
