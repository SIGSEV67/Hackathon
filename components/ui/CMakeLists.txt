idf_component_register(
    SRCS "ui.c" "screens.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh
    PRIV_REQUIRES board periph presence messages call esp_event
)
