/* 存储服务 - SPIFFS 与 microSD 卡管理
 * 管理内部 SPIFFS 文件系统（用于配置文件和人脸数据库）和
 * 外部 microSD 卡（用于录像存储）的挂载与同步。
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* 初始化内部 SPIFFS 文件系统 */
esp_err_t storage_service_init(void);
/* 挂载 microSD 卡（1-bit、10MHz 保守模式以保证稳定性） */
esp_err_t storage_service_start(void);
/* 同步所有文件流到存储介质 */
esp_err_t storage_service_sync(void);
/* 检查 microSD 卡是否已成功挂载 */
bool storage_service_sd_mounted(void);
/* 获取 microSD 挂载点路径 */
const char *storage_service_mount_point(void);