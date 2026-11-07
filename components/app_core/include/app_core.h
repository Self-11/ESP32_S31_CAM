/* 应用核心 - 系统状态管理与服务编排
 * 管理整个系统的生命周期状态（启动、就绪、流传输、OTA 维护等），
 * 协调各服务模块的初始化顺序和启动/停止逻辑。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 系统运行状态枚举 */
typedef enum {
    APP_STATE_BOOT = 0,              /* 上电启动 */
    APP_STATE_INITIALIZING,          /* 正在初始化各服务模块 */
    APP_STATE_READY,                 /* 就绪，可接收请求 */
    APP_STATE_STREAMING,             /* 正在传输视频流 */
    APP_STATE_RECORDING,             /* 正在录制 */
    APP_STATE_MAINTENANCE_OTA,       /* OTA 固件升级维护模式 */
    APP_STATE_FAULT,                 /* 故障状态 */
} app_state_t;

/* 系统初始化，依次调用所有子服务的 init 函数 */
esp_err_t app_core_init(void);
/* 系统启动，调用所有子服务的 start 函数，确认 OTA 镜像 */
esp_err_t app_core_start(void);
/* 获取当前系统状态（线程安全） */
app_state_t app_core_get_state(void);
/* 进入 OTA 维护模式：停止流/录制/检测，同步存储 */
esp_err_t app_core_enter_ota_mode(void);
/* 退出 OTA 模式：恢复媒体、录制、检测和网络 */
esp_err_t app_core_leave_ota_mode(void);

#ifdef __cplusplus
}
#endif