/* JPEG 硬件编码服务
 * 使用 ESP32-S31 硬件 JPEG 编码器将 RGB565 帧压缩为 JPEG，
 * 采用双缓冲机制实现无锁的编码-消费流水线。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* JPEG 帧数据，_slot 字段用于内部引用计数追踪 */
typedef struct {
    const uint8_t *data;            /* JPEG 数据指针 */
    size_t size;                    /* JPEG 数据长度 */
    uint16_t width;                 /* 图像宽度 */
    uint16_t height;                /* 图像高度 */
    uint32_t sequence;              /* 帧序号 */
    int64_t timestamp_us;           /* 时间戳（微秒） */
    uint8_t _slot;                  /* 内部槽位索引 */
} jpeg_frame_t;

/* 初始化硬件 JPEG 编码器并分配双缓冲区 */
esp_err_t jpeg_service_init(uint16_t width, uint16_t height, uint8_t quality);
/* 重配置 JPEG 编码器（分辨率/质量），等待现有帧释放 */
esp_err_t jpeg_service_reconfigure(uint16_t width, uint16_t height, uint8_t quality,
                                   uint32_t timeout_ms);
/* 将 RGB565 像素数据编码为 JPEG */
esp_err_t jpeg_service_encode_rgb565(const void *pixels, size_t size, int64_t timestamp_us);
/* 获取最新编码完成的 JPEG 帧（引用计数+1） */
bool jpeg_service_acquire_latest(jpeg_frame_t *frame);
/* 释放 JPEG 帧（引用计数-1） */
void jpeg_service_release(jpeg_frame_t *frame);
/* 已成功编码的帧数 */
uint32_t jpeg_service_encoded_count(void);
/* 因缓冲区满而丢弃的帧数 */
uint32_t jpeg_service_dropped_count(void);