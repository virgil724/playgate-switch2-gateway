idf_component_register(
    SRCS "input_transports.c" "serial_parser.c" "serial_input.c" "udp_input.c"
    INCLUDE_DIRS "include"
    REQUIRES controller_core switch2_controller esp_driver_uart esp_driver_usb_serial_jtag esp_event esp_netif esp_timer esp_wifi lwip nvs_flash
)
