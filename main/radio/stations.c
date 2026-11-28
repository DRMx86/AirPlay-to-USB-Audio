#include "stations.h"

/* The first entry is played at boot.  Add more stations below it. */
const radio_station_t g_stations[] = {
    {"Monster RX", "https://in-icecast.eradioportal.com:8443/monsterrrx"},
    {"Win Radio 91.5", "https://stream.zeno.fm/2ss1hgnu6hhvv"},
    {"Magic 89.9", "https://stream-161.zeno.fm/sh37pvfd938uv"},
    {"Asia Dream Radio (Japan Hits)", "https://quincy.torontocast.com:2020/;"},
    {"J-Rock PowerPlay", "https://kathy.torontocast.com:3340/;"},
    {"HotHitz (80's Hits)", "http://216.245.210.78:9900/stream"},
    {"POWERHITZ Ultimate 80's",
     "http://216.235.89.134:80/ultimate80s?fromyp=true"},
};

const size_t g_station_count = sizeof(g_stations) / sizeof(g_stations[0]);
