idf_component_register(
    SRCS "call.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh esp_event
    PRIV_REQUIRES periph adpcm presence
)
