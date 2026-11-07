/* OTA 固件升级服务 - ESP-IDF 原生 OTA 接口封装
 * 支持固件镜像的分块写入、完成确认和回滚取消。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 OTA 后端 */
esp_err_t ota_service_init(void);
/* 确认当前运行的 OTA 镜像有效，取消回滚定时器 */
esp_err_t ota_service_confirm_running(void);
/* 开始 OTA 写入会话，指定总镜像大小 */
esp_err_t ota_service_begin(size_t image_size);
/* 写入一块 OTA 数据 */
esp_err_t ota_service_write(const uint8_t *data, size_t length);
/* 完成 OTA 写入并设置启动分区 */
esp_err_t ota_service_finish(void);
/* 中止当前 OTA 会话 */
void ota_service_abort(void);