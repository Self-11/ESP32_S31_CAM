idf_component_register(
    SRCS "face_recognition_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer heap freertos espressif__esp-dl espressif__human_face_recognition
)