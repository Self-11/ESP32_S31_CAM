idf_component_register(
    SRCS "face_detection_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer heap freertos espressif__human_face_detect
)

