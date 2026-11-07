idf_component_register(
    SRCS "settings_service.c"
    INCLUDE_DIRS "include"
    REQUIRES board_service esp_common lvgl network_service
)
