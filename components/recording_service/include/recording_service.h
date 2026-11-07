/* SD 卡录像服务 - MJPEG AVI 格式录制
 * 将 JPEG 帧封装为 AVI 容器写入 microSD 卡，
 * 支持按时间和分辨率自动分段，以及磁盘空间不足时自动删除旧文件。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 启动 SD 卡录像任务 */
esp_err_t recording_service_start(void);
/* 停止录像并完成 AVI 文件封包 */
esp_err_t recording_service_stop(void);
/* 检查录像任务是否正在运行 */
bool recording_service_is_running(void);
/* 获取已录制的总帧数 */
uint32_t recording_service_frame_count(void);
/* 获取当前录像文件路径 */
const char *recording_service_current_file(void);