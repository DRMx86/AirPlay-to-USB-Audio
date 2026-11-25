set(SRC_FILES
    "main.c"
    "audio/audio_input_i2s.c"
    "audio/audio_output_usb_host.c"
    "audio/audio_resample.c"
)

set(INC_DIRS
    "."
    "audio"
)

set(DEPS
    driver
    esp_driver_i2s
    esp_driver_gpio
    esp_ringbuf
    esp_timer
    boards
    audio-resampler
    usb
)

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)
