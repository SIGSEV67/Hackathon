idf_component_register(
    SRCS "display.c" "audio.c" "buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_event
    PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_i2s esp_driver_gpio
)
