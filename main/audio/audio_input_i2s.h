#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * I2S audio input.
 *
 * Captures a stereo I2S stream (slave or master, 16/32-bit slots), converts it
 * to 16-bit PCM, resamples 44.1 kHz -> 48 kHz when configured, and pushes it
 * to the USB output with audio_output_write().
 */

typedef struct {
  bool active;              /* data has been received recently */
  uint32_t frames_captured; /* stereo frames read from I2S (wraps) */
  uint32_t read_timeouts; /* reads that returned no data (no clock / source) */
} audio_input_stats_t;

/**
 * Create and configure the I2S RX channel (does not start capturing).
 */
esp_err_t audio_input_init(void);

/**
 * Enable the I2S channel and start the capture task.
 */
esp_err_t audio_input_start(void);

/**
 * Snapshot of capture counters.
 */
void audio_input_get_stats(audio_input_stats_t *stats);
