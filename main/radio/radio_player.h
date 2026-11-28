#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Web radio player.
 *
 * Streams an Icecast/Shoutcast station over HTTP(S), decodes MP3 or AAC and
 * pushes 16-bit stereo PCM to the USB DAC with audio_output_write().
 *
 *   network task -> byte buffer (PSRAM) -> decoder task -> USB output FIFO
 *
 * The byte buffer rides out Wi-Fi jitter; the decoder is paced by the USB FIFO
 * so the server is read at playback speed.
 */

typedef struct {
  const char *station;  /* name of the selected station */
  bool streaming;       /* connected to the server and receiving data */
  const char *codec;    /* "MP3", "AAC" or "-" */
  uint32_t sample_rate; /* sample rate of the stream, 0 if not decoded yet */
  uint8_t channels;
  uint32_t buffered_bytes; /* compressed bytes waiting for the decoder */
  uint32_t capacity_bytes; /* size of that buffer */
  uint32_t reconnects;     /* connection attempts after the first */
  uint32_t decode_errors;
  char title[96]; /* current track title from ICY metadata, if any */
} radio_player_stats_t;

/**
 * Allocate buffers and start the network and decoder tasks.  Wi-Fi must be
 * started separately; the player waits for an IP address.
 */
esp_err_t radio_player_start(void);

/**
 * Switch to the station at this index of g_stations[].
 */
void radio_player_select(size_t index);

/**
 * Switch to the next station, wrapping around.
 */
void radio_player_next(void);

/**
 * Snapshot of player state.
 */
void radio_player_get_stats(radio_player_stats_t *stats);
