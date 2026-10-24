idf_component_register(
    SRCS "easter.c" "konami.c" "serial.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mesh periph ui esp_event esp_system esp_hw_support esp_driver_usb_serial_jtag
)
