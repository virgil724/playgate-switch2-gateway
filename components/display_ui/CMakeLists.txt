idf_component_register(
    SRCS "display_ui.c" "display_ui_port.c" "display_ui_view.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_lcd esp_driver_spi esp_driver_ledc
             controller_core switch2_controller input_transports esp_timer
)

# LVGL 8.4 is third-party code built with IDF's -Wall -Werror -Wextra and -std=gnu23;
# keep its warnings from failing this project's build.
if(TARGET __idf_lvgl)
    target_compile_options(__idf_lvgl PRIVATE -Wno-error)
    set_target_properties(__idf_lvgl PROPERTIES C_STANDARD 17)
endif()
