#include "stations.h"

/* The first entry is played at boot.  Add more stations below it. */
const radio_station_t g_stations[] = {
    {"Monster RX", "https://in-icecast.eradioportal.com:8443/monsterrrx"},
    /* {"Another station", "http://example.com:8000/stream"}, */
};

const size_t g_station_count = sizeof(g_stations) / sizeof(g_stations[0]);
