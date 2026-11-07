idf_component_register(
    SRCS "board_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp32_s31_korvo_1 esp_cam_sensor esp_video
)
