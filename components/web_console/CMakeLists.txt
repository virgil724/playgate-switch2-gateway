idf_component_register(
    SRCS "web_console.c"
    INCLUDE_DIRS "include"
    REQUIRES controller_core esp_http_server esp_timer
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../../tools/gamepad.html"
)
