/* OTA 固件升级服务实现
 * 使用 ESP-IDF 原生 OTA API 管理固件分区的写入和切换。
 * 支持 begin/write/finish/abort 标准 OTA 工作流。
 */

#include "ota_service.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_service";
static bool s_initialized;
static esp_ota_handle_t s_handle;       /* OTA 写入句柄 */
static const esp_partition_t *s_partition; /* 目标分区 */
static bool s_active;                   /* OTA 会话是否活跃 */

/* [OTA服务 初始化] 获取 OTA 数据分区并初始化目标分区指针 */
esp_err_t ota_service_init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "ESP-IDF OTA backend initialized");
    return ESP_OK;
}

/* 启动时调用：检查当前运行分区是否为待验证状态，
 * 若是则标记为有效并取消回滚 */
esp_err_t ota_service_confirm_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return ESP_OK;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "confirming booted OTA image in %s", running->label);
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

/* [OTA服务 开始] 打开 OTA 分区并准备写入 */
esp_err_t ota_service_begin(size_t image_size)
{
    if (!s_initialized || s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 获取下一个可用的 OTA 更新分区 */
    s_partition = esp_ota_get_next_update_partition(NULL);
    if (s_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_begin(s_partition, image_size, &s_handle);
    if (err == ESP_OK) {
        s_active = true;
        ESP_LOGI(TAG, "OTA target: %s", s_partition->label);
    }
    return err;
}

/* [OTA服务 写入] 将固件数据块写入 OTA 分区 */
esp_err_t ota_service_write(const uint8_t *data, size_t length)
{
    if (!s_active || data == NULL || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ota_write(s_handle, data, length);
}

/* 结束 OTA 写入并设置该分区为下次启动分区 */
esp_err_t ota_service_finish(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(s_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_partition);  /* 标记为启动分区 */
    }
    s_active = false;
    s_partition = NULL;
    return err;
}

/* 中止 OTA：放弃已写入数据，释放资源 */
void ota_service_abort(void)
{
    if (s_active) {
        esp_ota_abort(s_handle);
        s_active = false;
        s_partition = NULL;
    }
}