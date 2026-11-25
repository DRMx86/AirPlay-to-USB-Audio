#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * USB Audio Class (UAC) output.
 *
 * The ESP32 enumerates as a USB audio input device (a "microphone") on the
 * connected host.  Audio pushed in with audio_output_write() is delivered to
 * the host over the isochronous endpoint.  The stream is always 16-bit
 * stereo at CONFIG_OUTPUT_SAMPLE_RATE_HZ.
 */

typedef struct {
  bool streaming;          /* ring buffer primed and being sent to the host */
  uint32_t fill_bytes;     /* bytes currently buffered */
  uint32_t capacity_bytes; /* total ring buffer size */
  uint32_t overruns;       /* times the buffer was full and audio was dropped */
  uint32_t underruns;      /* times the host asked for data we did not have */
} audio_output_stats_t;

/**
 * Create the ring buffer and bring up the USB UAC device.
 */
esp_err_t audio_output_init(void);

/**
 * Queue interleaved stereo 16-bit PCM for the USB host.
 *
 * Never blocks.  If the buffer is full the excess frames are dropped (this is
 * how slow clock drift between the I2S source and the USB host is absorbed).
 *
 * @param pcm     Interleaved L,R int16 samples
 * @param frames  Number of stereo frames (not samples, not bytes)
 * @return number of frames actually queued
 */
size_t audio_output_write(const int16_t *pcm, size_t frames);

/**
 * Drop all buffered audio and re-prime before streaming again.
 */
void audio_output_flush(void);

/**
 * Snapshot of buffer state and error counters.
 */
void audio_output_get_stats(audio_output_stats_t *stats);
