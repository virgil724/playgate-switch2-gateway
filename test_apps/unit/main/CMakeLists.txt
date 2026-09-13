idf_component_register(
    SRCS
        "test_app_main.c"
        "test_controller_core.c"
        "test_switch2_protocol.c"
        "test_switch2_commands.c"
        "test_switch2_pairing.c"
        "test_switch2_advertising.c"
        "test_serial_parser.c"
    INCLUDE_DIRS "."
    REQUIRES unity controller_core switch2_controller input_transports
    WHOLE_ARCHIVE
)
