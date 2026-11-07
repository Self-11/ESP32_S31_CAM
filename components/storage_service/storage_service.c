/* 存储服务实现
 * 内部 SPIFFS 用于人脸数据库和配置文件的持久化存储；
 * 外部 microSD 使用保守的 1-bit 10MHz SDMMC 模式，
 * 避免在摄像头/LCD 同时工作时出现写入超时。
 */

#include "storage_service.h"

#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "bsp/esp32_s31_korvo_1.h"

static const char *TAG = "storage_service";
static bool s_initialized;
static bool s_sd_mounted;
static bool s_spiffs_mounted;

/* 初始化存储服务：挂载内部 SPIFFS 分区（人脸数据库 + 配置文件） */
esp_err_t storage_service_init(void)
{
    if (!s_initialized) {
        /* 挂载内部 SPIFFS 分区 */
        esp_err_t err = bsp_spiffs_mount();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "internal SPIFFS mount failed: %s", esp_err_to_name(err));
            return err;
        }
        s_spiffs_mounted = true;
        ESP_LOGI(TAG, "internal SPIFFS mounted at %s", CONFIG_BSP_SPIFFS_MOUNT_POINT);
        ESP_LOGI(TAG, "storage service initialized; mount point: %s", BSP_SD_MOUNT_POINT);
        s_initialized = true;
    }
    return ESP_OK;
}

/* 挂载 microSD 卡：使用保守的 1-bit 10MHz 模式避免与摄像头/LCD 冲突。
 * SD 卡不是系统必需的，挂载失败时系统仍可正常运行（仅禁用录像功能）。 */
esp_err_t storage_service_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sd_mounted) {
        return ESP_OK;
    }

    /* BSP 默认使用 4-bit 高速 SDMMC 总线。但本摄像头板在该模式下
     * 摄像头/LCD 同时工作时会产生 ESP_ERR_TIMEOUT，因此降级为
     * 保守的 1-bit 10MHz 总线，因为录制带宽需求不高。 */
    sdmmc_host_t host = {0};
    sdmmc_slot_config_t slot = {0};
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    host.max_freq_khz = 10000;      /* 降低频率至 10MHz */
    slot.width = 1;                 /* 使用 1-bit 模式 */
    slot.d1 = GPIO_NUM_NC;          /* 未使用的数据线悬空 */
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    bsp_sdcard_cfg_t sd_config = {
        .host = &host,
        .slot.sdmmc = &slot,
    };
    esp_err_t err = bsp_sdcard_sdmmc_mount(&sd_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD not mounted (%s); capture and recording remain disabled",
                 esp_err_to_name(err));
        return ESP_OK;              /* SD 卡不是必需的，允许系统继续运行 */
    }

    s_sd_mounted = true;
    sdmmc_card_t *card = bsp_sdcard_get_handle();
    if (card != NULL) {
        ESP_LOGI(TAG, "microSD mounted: %s, capacity %llu MiB (1-bit, 10 MHz)", card->cid.name,
                 (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024ULL * 1024ULL)));
    }
    return ESP_OK;
}

/* 同步所有打开的文件流，确保数据写入物理存储 */
esp_err_t storage_service_sync(void)
{
    if (s_spiffs_mounted) {
        fflush(NULL);               /* 刷新所有 SPIFFS 文件流 */
    }
    if (s_sd_mounted) {
        fflush(NULL);               /* 刷新所有 SD 卡文件流 */
        ESP_LOGI(TAG, "microSD file streams flushed");
    }
    return ESP_OK;
}

/* [存储服务 SD状态] 查询 SD 卡是否已成功挂载 */
bool storage_service_sd_mounted(void)
{
    return s_sd_mounted;
}

const char *storage_service_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}