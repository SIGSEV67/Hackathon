idf_component_register(
    SRCS "main.c" "controls.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board periph noise radio link mesh presence messages web ui esp_event esp_timer nvs_flash
)
