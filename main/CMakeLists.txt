idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board periph noise radio link mesh presence messages web esp_event nvs_flash
)
