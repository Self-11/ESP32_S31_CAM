idf_component_register(
    SRCS "jpeg_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_jpeg esp_timer freertos
)
