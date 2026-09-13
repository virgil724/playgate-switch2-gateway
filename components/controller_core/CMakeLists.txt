idf_component_register(
    SRCS "controller_core.c" "gateway_status.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)
