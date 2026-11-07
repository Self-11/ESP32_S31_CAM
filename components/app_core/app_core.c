/* 应用核心 - 系统状态管理与服务编排实现
 * 使用互斥锁保护全局状态，按依赖顺序初始化和启动各服务模块。
 * 初始化顺序：板级 -> 存储 -> 人脸检测 -> 媒体 -> 网络 -> 设置 -> OTA
 * 启动顺序：板级 -> 存储 -> 人脸识别 -> 媒体 ->（SD 录制）-> 网络 -> OTA 确认
 */

#include "app_core.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "board_service.h"
#include "face_detection_service.h"
#include "face_recognition_service.h"
#include "media_service.h"
#include "network_service.h"
#include "ota_service.h"
#include "recording_service.h"
#include "settings_service.h"
#include "storage_service.h"

static const char *TAG = "app_core";
static SemaphoreHandle_t s_lock;        /* 状态互斥锁 */
static app_state_t s_state = APP_STATE_BOOT;

/* 线程安全地设置系统状态 */
static void set_state(app_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "state -> %d", state);
}

/* [应用核心 初始化] 按依赖顺序初始化所有服务模块：
 * 板级 → 存储 → 人脸检测 → 媒体 → 网络 → 设置 → OTA */
esp_err_t app_core_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;              /* 已初始化，幂等操作 */
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "state mutex allocation failed");
    set_state(APP_STATE_INITIALIZING);

    /* 按依赖顺序初始化各服务模块 */
    ESP_RETURN_ON_ERROR(board_service_init(), TAG, "board service init failed");
    ESP_RETURN_ON_ERROR(storage_service_init(), TAG, "storage service init failed");
    ESP_RETURN_ON_ERROR(face_detection_service_init(), TAG, "face detection init failed");
    ESP_RETURN_ON_ERROR(media_service_init(), TAG, "media service init failed");
    ESP_RETURN_ON_ERROR(network_service_init(), TAG, "network service init failed");
    ESP_RETURN_ON_ERROR(settings_service_init(), TAG, "settings service init failed");
    ESP_RETURN_ON_ERROR(ota_service_init(), TAG, "OTA service init failed");
    return ESP_OK;
}

/* [应用核心 启动] 按依赖顺序启动所有服务：
 * 板级 → 存储 → 人脸识别 → 媒体 →（SD 录制）→ 网络 → OTA 确认 */
esp_err_t app_core_start(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "app core is not initialized");
    ESP_RETURN_ON_ERROR(board_service_start(), TAG, "board service start failed");
    ESP_RETURN_ON_ERROR(storage_service_start(), TAG, "storage service start failed");
    ESP_RETURN_ON_ERROR(face_recognition_service_init(CONFIG_FACE_RECOGNITION_DB_PATH), TAG, "face recognition init failed");
    ESP_RETURN_ON_ERROR(media_service_start(), TAG, "media service start failed");
    if (storage_service_sd_mounted()) {
        ESP_RETURN_ON_ERROR(recording_service_start(), TAG, "SD recording start failed");
    }
    ESP_RETURN_ON_ERROR(network_service_start(), TAG, "network service start failed");
    set_state(APP_STATE_READY);
    /* 确认当前运行的 OTA 镜像有效，取消回滚 */
    ESP_RETURN_ON_ERROR(ota_service_confirm_running(), TAG, "OTA image confirmation failed");
    return ESP_OK;
}

/* [应用核心 状态查询] 获取当前系统状态（线程安全） */
app_state_t app_core_get_state(void)
{
    if (s_lock == NULL) {
        return APP_STATE_BOOT;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    app_state_t state = s_state;
    xSemaphoreGive(s_lock);
    return state;
}

/* 进入 OTA 模式：暂停所有非必要服务以释放内存和 CPU，
 * 仅保留 HTTP 服务器用于接收固件上传。 */
esp_err_t app_core_enter_ota_mode(void)
{
    ESP_RETURN_ON_FALSE(app_core_get_state() != APP_STATE_MAINTENANCE_OTA,
                        ESP_ERR_INVALID_STATE, TAG, "OTA mode already active");

    ESP_RETURN_ON_ERROR(network_service_quiesce(), TAG, "network quiesce failed");
    ESP_RETURN_ON_ERROR(recording_service_stop(), TAG, "SD recording stop failed");
    ESP_RETURN_ON_ERROR(face_detection_service_set_enabled(false), TAG,
                        "face detection pause failed");
    ESP_RETURN_ON_ERROR(face_recognition_service_set_enabled(false), TAG,
                        "face recognition pause failed");
    ESP_RETURN_ON_ERROR(media_service_stop(), TAG, "media stop failed");
    ESP_RETURN_ON_ERROR(storage_service_sync(), TAG, "storage sync failed");
    set_state(APP_STATE_MAINTENANCE_OTA);
    return ESP_OK;
}

/* [应用核心 退出OTA] 恢复所有暂停的服务 */
esp_err_t app_core_leave_ota_mode(void)
{
    ESP_RETURN_ON_FALSE(app_core_get_state() == APP_STATE_MAINTENANCE_OTA,
                        ESP_ERR_INVALID_STATE, TAG, "OTA mode is not active");
    ESP_RETURN_ON_ERROR(media_service_start(), TAG, "media restart failed");
    if (storage_service_sd_mounted()) {
        ESP_RETURN_ON_ERROR(recording_service_start(), TAG, "SD recording restart failed");
    }
    ESP_RETURN_ON_ERROR(face_detection_service_set_enabled(true), TAG,
                        "face detection resume failed");
    ESP_RETURN_ON_ERROR(face_recognition_service_set_enabled(true), TAG,
                        "face recognition resume failed");
    ESP_RETURN_ON_ERROR(network_service_resume(), TAG, "network resume failed");
    set_state(APP_STATE_READY);
    return ESP_OK;
}