idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board periph noise radio link mesh presence esp_event nvs_flash
)
