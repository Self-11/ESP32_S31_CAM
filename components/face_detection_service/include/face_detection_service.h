/* 人脸检测服务 - ESP-DL MSRMNP 模型封装
 * 基于 ESP-DL 的双阶段人脸检测（MSRMNP），在独立的检测任务中异步执行推理。
 * 支持最多 4 个人脸 + 5 点关键点（左右眼、鼻尖、左右嘴角）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_DETECTION_MAX_RESULTS 4U         /* 最多检测到的人脸数 */

/* 单个人脸检测结果 */
typedef struct {
    uint16_t x;                              /* 边界框左上角 x */
    uint16_t y;                              /* 边界框左上角 y */
    uint16_t width;                          /* 边界框宽度 */
    uint16_t height;                         /* 边界框高度 */
    uint16_t score_x10000;                   /* 置信度 * 10000 */
    int16_t keypoint[10];                    /* 5 点关键点 (x,y) 对 */
} face_detection_box_t;

/* 检测结果集 */
typedef struct {
    uint32_t generation;                     /* 结果代数，用于判断是否更新 */
    uint32_t source_sequence;                /* 源帧序号 */
    uint16_t source_width;                   /* 源帧宽度 */
    uint16_t source_height;                  /* 源帧高度 */
    uint8_t count;                           /* 检测到的人脸数量 */
    face_detection_box_t boxes[FACE_DETECTION_MAX_RESULTS];
} face_detection_results_t;

/* 初始化人脸检测模型 */
esp_err_t face_detection_service_init(void);
/* 启用人脸检测 */
esp_err_t face_detection_service_set_enabled(bool enabled);
/* 人脸检测是否启用 */
bool face_detection_service_is_enabled(void);
/* 模型是否已加载完成 */
bool face_detection_service_is_ready(void);

/* 异步提交 RGB565 帧进行人脸检测。
 * 将源帧中心裁剪并降采样到 224x224 后送入双缓冲队列，
 * 仅保留最新待处理帧，从不阻塞采集端。 */
esp_err_t face_detection_service_submit_rgb565(const uint16_t *pixels,
                                               uint16_t width,
                                               uint16_t height,
                                               uint32_t source_sequence);
/* 获取最新异步检测结果 */
bool face_detection_service_get_latest(face_detection_results_t *results);
/* 最近一次推理耗时（毫秒） */
uint32_t face_detection_service_inference_ms(void);
/* 因推理忙而丢弃的帧数 */
uint32_t face_detection_service_dropped_frames(void);

/* 同步检测接口（阻塞调用，用于注册等低延迟场景） */
esp_err_t face_detection_service_detect_sync(const uint16_t *pixels,
                                             uint16_t width, uint16_t height,
                                             face_detection_results_t *out);

#ifdef __cplusplus
}
#endif