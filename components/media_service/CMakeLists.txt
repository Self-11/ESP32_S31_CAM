idf_component_register(
    SRCS "media_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos nvs_flash board_service face_detection_service face_recognition_service jpeg_service esp_video esp_timer lvgl__lvgl
)