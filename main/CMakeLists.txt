# 主组件 CMake 构建脚本
# 依赖 app_core（应用核心）、board_service（板级服务）、media_service（媒体服务）、
# network_service（网络服务）、storage_service（存储服务）、ota_service（OTA 升级服务）、
# stream_protocol（流协议）
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_core board_service media_service network_service storage_service ota_service stream_protocol
)