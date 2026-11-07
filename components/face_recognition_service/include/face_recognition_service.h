/* 人脸识别服务 - ESP-DL MFN 模型封装
 * 基于 MobileFaceNet (MFN) 模型进行人脸特征提取和余弦相似度比对。
 * 人脸库以 ID 为键存储在磁盘（SPIFFS），支持注册/删除/清空操作。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化人脸识别模型并加载已注册的人脸库 */
esp_err_t face_recognition_service_init(const char *db_path);
/* 启用人脸识别 */
esp_err_t face_recognition_service_set_enabled(bool enabled);
/* 人脸识别是否启用 */
bool face_recognition_service_is_enabled(void);
/* 模型是否已加载完成 */
bool face_recognition_service_is_ready(void);

/* 对已检测到的人脸进行识别（基于关键点对齐） */
esp_err_t face_recognition_service_recognize(const uint16_t *pixels,
                                             uint16_t width,
                                             uint16_t height,
                                             const int16_t *keypoints,
                                             uint16_t *out_id,
                                             float *out_confidence);

/* 注册新的人脸到数据库 */
esp_err_t face_recognition_service_enroll(const uint16_t *pixels,
                                          uint16_t width,
                                          uint16_t height,
                                          uint16_t face_x,
                                          uint16_t face_y,
                                          uint16_t face_w,
                                          uint16_t face_h,
                                          const int16_t *keypoints,
                                          uint16_t *out_id);

/* 删除指定 ID 的人脸特征 */
esp_err_t face_recognition_service_delete(uint16_t id);
/* 清空所有人脸特征 */
esp_err_t face_recognition_service_clear(void);
/* 已注册的人脸数量 */
uint16_t face_recognition_service_user_count(void);

/* 获取当前识别阈值（0.0-1.0） */
float face_recognition_service_threshold_get(void);
/* 设置识别阈值 */
void face_recognition_service_threshold_set(float threshold);

/* 最近一次推理耗时（毫秒） */
uint32_t face_recognition_service_inference_ms(void);

#ifdef __cplusplus
}
#endif