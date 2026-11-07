/* 系统设置服务 - LVGL Wi-Fi 配置界面
 * 在 LCD 上提供触屏 Wi-Fi 设置页面，包含 SSID/密码输入、
 * 网络状态实时显示和设备 ID 查看功能。
 */

#pragma once

#include "esp_err.h"

/* 初始化 NVS 分区并恢复默认设置 */
esp_err_t settings_service_init(void);