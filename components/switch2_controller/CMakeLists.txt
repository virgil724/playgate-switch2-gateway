idf_component_register(
    SRCS "switch2_controller.c" "switch2_gatt.c" "switch2_protocol.c" "switch2_pairing.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES controller_core bt nvs_flash esp_timer mbedtls
)
