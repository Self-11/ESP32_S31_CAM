/* ESP32-S31 视觉系统 - 主入口
 * 基于 ESP32-S31 Korvo-1 开发板，集成 OV3660 摄像头、RGB LCD 显示、
 * 人脸检测与识别、MJPEG/TCP 视频流、SD 卡录像和 OTA 固件升级等功能。
 */

#include "esp_log.h"

#include "app_core.h"

static const char *TAG = "app_main";

/* FreeRTOS 应用主入口，由 ESP-IDF 框架在系统初始化完成后调用 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S31 vision system boot");

    /* 初始化所有服务模块：板级、存储、人脸检测、媒体、网络、设置、OTA */
    esp_err_t err = app_core_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_core_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* 启动所有服务模块，进入就绪状态 */
    err = app_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_core_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "system entered READY state");
}