idf_component_register(
    SRCS "ranging.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh
    PRIV_REQUIRES esp_wifi esp_event
)
