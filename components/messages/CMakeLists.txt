idf_component_register(
    SRCS "messages.c" "store.c" "alerts.c" "voice.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh esp_event
    PRIV_REQUIRES periph adpcm heap
)
