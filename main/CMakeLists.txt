idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES periph noise radio link mesh esp_event nvs_flash
)
