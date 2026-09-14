idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES controller_core switch2_controller input_transports web_console display_ui nvs_flash
)
