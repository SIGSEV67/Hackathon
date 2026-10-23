idf_component_register(
    SRCS "ui.c" "screens.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board periph mesh presence messages esp_event
)
