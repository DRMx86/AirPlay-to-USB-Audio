/**
 * Web radio -> USB DAC.
 *
 * Streams an Icecast station over Wi-Fi, decodes it and plays it on a USB DAC
 * dongle plugged into the ESP32-S3's native USB (host) port.
 */

#include "audio_output.h"
#include "iot_board.h"
#include "radio_player.h"
#include "wifi_sta.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define STATS_PERIOD_MS 5000

static void log_stats(void) {
  radio_player_stats_t radio;
  audio_output_stats_t out;
  radio_player_get_stats(&radio);
  audio_output_get_stats(&out);

  ESP_LOGI(TAG,
           "Wi-Fi: %s | Radio: \"%s\" %s, %s %lu Hz %uch, net buffer %lu%%, "
           "reconnects %lu, decode errors %lu | USB DAC: %s, buffer %lu%%, "
           "overruns %lu, underruns %lu",
           wifi_sta_is_connected() ? "connected" : "down", radio.station,
           radio.streaming ? "streaming" : "idle", radio.codec,
           (unsigned long)radio.sample_rate, radio.channels,
           (unsigned long)(radio.capacity_bytes
                               ? (uint64_t)radio.buffered_bytes * 100 /
                                     radio.capacity_bytes
                               : 0),
           (unsigned long)radio.reconnects, (unsigned long)radio.decode_errors,
           !out.connected ? "not connected"
                          : (out.streaming ? "streaming" : "priming"),
           (unsigned long)(out.capacity_bytes
                               ? out.fill_bytes * 100 / out.capacity_bytes
                               : 0),
           (unsigned long)out.overruns, (unsigned long)out.underruns);
}

#if CONFIG_RADIO_NEXT_BUTTON_GPIO >= 0
/* Pressing the button (to GND) switches to the next station. */
static void button_task(void *arg) {
  (void)arg;
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_RADIO_NEXT_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);

  int last = 1;
  while (true) {
    int level = gpio_get_level(CONFIG_RADIO_NEXT_BUTTON_GPIO);
    if (last == 1 && level == 0) {
      radio_player_next();
      vTaskDelay(pdMS_TO_TICKS(300)); /* debounce */
    }
    last = level;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
#endif

void app_main(void) {
  ESP_LOGI(TAG, "Web radio -> USB DAC, board: %s", iot_board_get_info());

  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  /* USB first so the host can start enumerating while Wi-Fi comes up. */
  ESP_ERROR_CHECK(audio_output_init());

  err = wifi_sta_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
  }

  err = radio_player_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Radio player start failed: %s", esp_err_to_name(err));
  }

#if CONFIG_RADIO_NEXT_BUTTON_GPIO >= 0
  xTaskCreate(button_task, "radio_btn", 2048, NULL, 3, NULL);
#endif

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
    log_stats();
  }
}
