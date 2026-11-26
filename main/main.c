/**
 * I2S -> USB audio bridge.
 *
 * Audio arrives on an I2S input (e.g. from an ADC, S/PDIF receiver or another
 * board) and is presented to a USB host as a USB Audio Class input device.
 */

#include "audio_input_i2s.h"
#include "audio_output.h"
#include "iot_board.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define STATS_PERIOD_MS 5000

static void log_stats(uint32_t *last_frames) {
  audio_input_stats_t in;
  audio_output_stats_t out;
  audio_input_get_stats(&in);
  audio_output_get_stats(&out);

  uint32_t delta = in.frames_captured - *last_frames;
  *last_frames = in.frames_captured;

  ESP_LOGI(TAG,
           "I2S: %s, %lu frames/%ds, %lu empty reads | USB DAC: %s, buffer "
           "%lu%%, overruns %lu, underruns %lu",
           in.active ? "active" : "idle", (unsigned long)delta,
           STATS_PERIOD_MS / 1000, (unsigned long)in.read_timeouts,
           !out.connected ? "not connected"
                          : (out.streaming ? "streaming" : "priming"),
           (unsigned long)(out.capacity_bytes
                               ? out.fill_bytes * 100 / out.capacity_bytes
                               : 0),
           (unsigned long)out.overruns, (unsigned long)out.underruns);
}

void app_main(void) {
  ESP_LOGI(TAG, "I2S -> USB DAC bridge, board: %s", iot_board_get_info());

  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  /* USB first so the host can start enumerating while I2S comes up. */
  ESP_ERROR_CHECK(audio_output_init());
  ESP_ERROR_CHECK(audio_input_init());
  ESP_ERROR_CHECK(audio_input_start());

  uint32_t last_frames = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
    log_stats(&last_frames);
  }
}
