idf_component_register(
    SRCS "network_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_event esp_http_client esp_http_server esp_netif esp_wifi face_detection_service freertos jpeg_service lwip
             media_service nvs_flash ota_service recording_service storage_service stream_protocol
)
