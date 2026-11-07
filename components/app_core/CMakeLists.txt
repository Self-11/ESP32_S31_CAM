idf_component_register(
    SRCS "app_core.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_event board_service face_detection_service face_recognition_service media_service network_service recording_service settings_service storage_service ota_service
)
