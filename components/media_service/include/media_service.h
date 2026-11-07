/* 媒体服务 - 摄像头帧采集与处理编排
 * 作为视频流水线的核心调度层，整合摄像头采集、JPEG 编码、人脸检测/识别，
 * 并根据客户端请求（TCP/MJPEG/录像）将帧分发到对应的输出端。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MEDIA_FACE_RECOGNITION_MAX 4U         /* 最多识别人脸数 */
#define MEDIA_FACE_LIST_MAX 32U               /* 人脸库最大容量 */

/* 像素格式枚举 */
typedef enum {
    MEDIA_PIXEL_FORMAT_RGB565 = 0,           /* RGB565 原始像素 */
    MEDIA_PIXEL_FORMAT_JPEG,                 /* JPEG 压缩 */
} media_pixel_format_t;

/* 媒体模式，控制分辨率/格式/质量 */
typedef struct {
    uint16_t width;
    uint16_t height;
    media_pixel_format_t format;
    uint8_t jpeg_quality;                    /* JPEG 质量 (1-100) */
} media_mode_t;

/* 人脸识别结果（ID + 置信度） */
typedef struct {
    uint16_t id;
    float confidence;
} media_face_recognition_t;

/* 人脸库列表条目 */
typedef struct {
    uint16_t id;
    char name[24];
} media_face_list_entry_t;

/* 初始化媒体流水线 */
esp_err_t media_service_init(void);
/* 启动采集并开始帧推送 */
esp_err_t media_service_start(void);
/* 停止采集 */
esp_err_t media_service_stop(void);
/* 运行时设置媒体模式 */
esp_err_t media_service_set_mode(const media_mode_t *mode);
/* 获取当前媒体模式 */
esp_err_t media_service_get_mode(media_mode_t *mode);
/* 是否正在采集 */
bool media_service_is_running(void);
/* 已采集帧数 */
uint32_t media_service_frame_count(void);
/* 获取最新人脸识别结果 */
bool media_service_get_recognition_labels(media_face_recognition_t *out, uint8_t *out_count);
/* 注册当前检测到的人脸 */
esp_err_t media_service_enroll_face(uint8_t face_index);
/* 通过上传 RGB565 图像注册人脸 */
esp_err_t media_service_enroll_face_upload(const uint16_t *rgb565,
                                            uint16_t crop_w, uint16_t crop_h,
                                            uint16_t *out_id);
/* 设置人脸名称 */
void media_service_set_face_name(uint16_t id, const char *name);
/* 清空人脸名称映射 */
void media_service_clear_face_names(void);
/* 获取人脸库列表 */
esp_err_t media_service_get_face_list(media_face_list_entry_t *out, uint8_t *out_count);