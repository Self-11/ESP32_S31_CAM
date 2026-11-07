idf_component_register(
    SRCS "recording_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos jpeg_service media_service storage_service fatfs
)
