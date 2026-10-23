idf_component_register(
    SRCS "presence.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh esp_event
    PRIV_REQUIRES nvs_flash
)
