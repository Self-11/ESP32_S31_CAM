/* JPEG 硬件编码服务实现
 * 使用 ESP32-S31 硬件 JPEG 编码器，双缓冲设计：
 * - 一个槽位被编码器写入（active），另一个空闲等待编码
 * - 消费者通过 acquire/release 管理引用计数
 * - 当双缓冲均被占用时，新帧被丢弃
 */

#include "jpeg_service.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JPEG_BUFFER_COUNT 2U           /* 双缓冲 */

/* JPEG 缓冲槽，含引用计数用于多消费者安全释放 */
typedef struct {
    uint8_t *data;                     /* JPEG 数据缓冲区 */
    size_t capacity;                   /* 缓冲区容量 */
    size_t size;                       /* 当前有效数据长度 */
    uint32_t sequence;                 /* 帧序号 */
    int64_t timestamp_us;              /* 时间戳 */
    uint32_t references;               /* 引用计数 */
} jpeg_slot_t;

static const char *TAG = "jpeg_service";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static jpeg_encoder_handle_t s_encoder;   /* 硬件编码器句柄 */
static jpeg_encode_cfg_t s_config;        /* 编码器配置 */
static jpeg_slot_t s_slots[JPEG_BUFFER_COUNT]; /* 双缓冲槽 */
static uint16_t s_width;                  /* 当前编码宽度 */
static uint16_t s_height;                 /* 当前编码高度 */
static int8_t s_active_slot = -1;         /* 当前活跃槽位索引 */
static bool s_initialized;
static uint32_t s_encoded_count;          /* 成功编码计数 */
static uint32_t s_dropped_count;          /* 丢弃帧计数 */

/* 释放所有编码器资源（双缓冲区 + 硬件引擎） */
static void release_resources(void)
{
    for (size_t i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        free(s_slots[i].data);
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
    if (s_encoder != NULL) {
        jpeg_del_encoder_engine(s_encoder);
        s_encoder = NULL;
    }
}

/* 初始化硬件 JPEG 编码器，分配双缓冲区并配置编码参数。
 * 每块缓冲大小 = width * height * 2 * 3/4（JPG 通常 < RGB565 大小） */
esp_err_t jpeg_service_init(uint16_t width, uint16_t height, uint8_t quality)
{
    if (s_initialized) {
        return (width == s_width && height == s_height && quality == s_config.image_quality)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(width > 0 && height > 0 && quality >= 1 && quality <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "invalid JPEG configuration");

    jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = 1000,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine_config, &s_encoder), TAG,
                        "hardware JPEG encoder creation failed");

    const size_t requested_capacity = (size_t)width * height * 2U * 3U / 4U;
    jpeg_encode_memory_alloc_cfg_t memory_config = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    for (size_t i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        s_slots[i].data = jpeg_alloc_encoder_mem(requested_capacity, &memory_config,
                                                  &s_slots[i].capacity);
        if (s_slots[i].data == NULL) {
            release_resources();
            return ESP_ERR_NO_MEM;
        }
    }

    s_config = (jpeg_encode_cfg_t) {
        .height = height,
        .width = width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality,
        .pixel_reverse = false,
    };
    s_width = width;
    s_height = height;
    s_active_slot = -1;
    s_encoded_count = 0;
    s_dropped_count = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "hardware JPEG ready: %ux%u quality=%u, 2x%u-byte buffers",
             width, height, quality, (unsigned)s_slots[0].capacity);
    return ESP_OK;
}

/* 动态重新配置编码参数（分辨率/质量），等待所有消费者释放引用后生效。
 * 缓冲区必须已在初始化时分配足够容量，重配置仅更新编码参数和分辨率。 */
esp_err_t jpeg_service_reconfigure(uint16_t width, uint16_t height, uint8_t quality,
                                   uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "JPEG service is not initialized");
    ESP_RETURN_ON_FALSE(width > 0 && height > 0 && quality >= 1 && quality <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "invalid JPEG configuration");
    /* 缓冲区按最大启动分辨率分配，重配置时仅需检查容量 */
    const size_t required_capacity = (size_t)width * height * 2U * 3U / 4U;
    ESP_RETURN_ON_FALSE(required_capacity <= s_slots[0].capacity &&
                        required_capacity <= s_slots[1].capacity,
                        ESP_ERR_NO_MEM, TAG, "JPEG buffers are too small for %ux%u", width, height);

    /* 等待所有消费者释放引用 */
    uint32_t waited_ms = 0;
    while (true) {
        bool references_released;
        taskENTER_CRITICAL(&s_lock);
        references_released = s_slots[0].references == 0 && s_slots[1].references == 0;
        if (references_released) {
            s_active_slot = -1;
            s_width = width;
            s_height = height;
            s_config.width = width;
            s_config.height = height;
            s_config.image_quality = quality;
            s_slots[0].size = 0;
            s_slots[1].size = 0;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (references_released) {
            ESP_LOGI(TAG, "hardware JPEG reconfigured: %ux%u quality=%u", width, height, quality);
            return ESP_OK;
        }
        if (waited_ms >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
}

/* RGB565 帧硬件 JPEG 编码入口：
 * 1. 寻找空闲缓冲槽（非 active 且 refcount=0）
 * 2. 调用 ESP32-S31 硬件 JPEG 编码器
 * 3. 标记为新活跃槽位，递增帧序号
 * 如果所有槽位都被占用则丢弃当前帧 */
esp_err_t jpeg_service_encode_rgb565(const void *pixels, size_t size, int64_t timestamp_us)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "JPEG service is not initialized");
    ESP_RETURN_ON_FALSE(pixels != NULL && size >= (size_t)s_width * s_height * 2U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid RGB565 frame");

    /* 查找空闲的非活跃槽位 */
    int8_t target = -1;
    taskENTER_CRITICAL(&s_lock);
    for (int8_t i = 0; i < (int8_t)JPEG_BUFFER_COUNT; ++i) {
        if (i != s_active_slot && s_slots[i].references == 0) {
            target = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);

    if (target < 0) {                   /* 所有槽位被占用，丢弃帧 */
        ++s_dropped_count;
        return ESP_ERR_NOT_FINISHED;
    }

    /* 调用硬件 JPEG 编码器 */
    uint32_t encoded_size = 0;
    esp_err_t ret = jpeg_encoder_process(s_encoder, &s_config, pixels,
                                         (uint32_t)size, s_slots[target].data,
                                         (uint32_t)s_slots[target].capacity, &encoded_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 更新活跃槽位 */
    taskENTER_CRITICAL(&s_lock);
    s_slots[target].size = encoded_size;
    s_slots[target].timestamp_us = timestamp_us;
    s_slots[target].sequence = ++s_encoded_count;
    s_active_slot = target;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

/* 获取最新编码完成的 JPEG 帧（引用计数+1）。
 * 返回 false 表示当前无可用帧，消费者使用完毕后必须调用 jpeg_service_release()。 */
bool jpeg_service_acquire_latest(jpeg_frame_t *frame)
{
    if (!s_initialized || frame == NULL) {
        return false;
    }

    bool acquired = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_active_slot >= 0) {
        jpeg_slot_t *slot = &s_slots[s_active_slot];
        ++slot->references;
        *frame = (jpeg_frame_t) {
            .data = slot->data,
            .size = slot->size,
            .width = s_width,
            .height = s_height,
            .sequence = slot->sequence,
            .timestamp_us = slot->timestamp_us,
            ._slot = (uint8_t)s_active_slot,
        };
        acquired = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return acquired;
}

/* 释放 JPEG 帧引用（引用计数-1），允许该缓冲槽被新编码结果复用 */
void jpeg_service_release(jpeg_frame_t *frame)
{
    if (frame == NULL || frame->_slot >= JPEG_BUFFER_COUNT) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_slots[frame->_slot].references > 0) {
        --s_slots[frame->_slot].references;
    }
    taskEXIT_CRITICAL(&s_lock);
    memset(frame, 0, sizeof(*frame));
    frame->_slot = UINT8_MAX;
}

/* 成功编码帧计数 */
uint32_t jpeg_service_encoded_count(void)
{
    return s_encoded_count;
}

/* 丢弃帧计数（所有缓冲槽被占用时丢弃） */
uint32_t jpeg_service_dropped_count(void)
{
    return s_dropped_count;
}