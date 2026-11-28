#pragma once

#include <stddef.h>

/**
 * Web radio station list.
 *
 * To add a station, append an entry to g_stations[] in stations.c.  Streams
 * must be Icecast/Shoutcast over HTTP or HTTPS carrying MP3 or AAC (ADTS).
 */

typedef struct {
  const char *name;
  const char *url;
} radio_station_t;

extern const radio_station_t g_stations[];
extern const size_t g_station_count;
