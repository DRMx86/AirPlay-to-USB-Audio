set(SRC_FILES
    "main.c"
    "net/wifi_sta.c"
    "radio/radio_player.c"
    "radio/stations.c"
    "audio/audio_output_usb_host.c"
    "audio/audio_resample.c"
)

set(INC_DIRS
    "."
    "audio"
    "net"
    "radio"
)

set(DEPS
    driver
    esp_driver_gpio
    esp_timer
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp-tls
    mbedtls
    nvs_flash
    boards
    audio-resampler
    usb
    espressif__esp_audio_codec
)

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)
