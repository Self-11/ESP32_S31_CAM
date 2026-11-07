/* 流传输协议 - 自定义二进制帧协议
 * 基于魔数 0x53333156 ("S31V") 的 32 字节帧头 + CRC32 校验，
 * 支持 JPEG 帧传输、人脸检测结果、人脸注册/删除等控制消息。
 * 帧头格式（大端序）：
 *   [0-3]   magic (S31V)
 *   [4]     version
 *   [5]     type
 *   [6-7]   flags
 *   [8-11]  sequence
 *   [12-19] timestamp_us
 *   [20-21] width
 *   [22-23] height
 *   [24-27] payload_length
 *   [28-31] header_crc32
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STREAM_PROTOCOL_MAGIC 0x53333156u       /* S31V */
#define STREAM_PROTOCOL_VERSION 1u
#define STREAM_PROTOCOL_HEADER_SIZE 32u          /* 帧头固定 32 字节 */

/* 消息类型枚举 */
typedef enum {
    STREAM_MSG_HELLO = 1,           /* 握手消息 */
    STREAM_MSG_CAPABILITIES,        /* 能力声明 */
    STREAM_MSG_JPEG_FRAME,          /* JPEG 视频帧 */
    STREAM_MSG_SET_MODE,            /* 设置摄像头模式 */
    STREAM_MSG_START,               /* 开始传输 */
    STREAM_MSG_STOP,                /* 停止传输 */
    STREAM_MSG_SNAPSHOT,            /* 快照请求 */
    STREAM_MSG_RECORD_START,        /* 开始录像 */
    STREAM_MSG_RECORD_STOP,         /* 停止录像 */
    STREAM_MSG_STATUS,              /* 状态查询 */
    STREAM_MSG_ERROR,               /* 错误通知 */
    STREAM_MSG_FACE_RESULTS,        /* 人脸检测结果 */
    STREAM_MSG_FACE_ENROLL = 20,    /* 人脸上传注册 */
    STREAM_MSG_FACE_DELETE,         /* 删除人脸 */
    STREAM_MSG_FACE_CLEAR,          /* 清空人脸库 */
    STREAM_MSG_FACE_LIST,           /* 查询人脸列表 */
    STREAM_MSG_FACE_ENROLL_ACK = 24,/* 注册确认 */
    STREAM_MSG_FACE_LIST_REPLY = 25,/* 人脸列表回复 */
    STREAM_MSG_FACE_THRESHOLD_SET = 26, /* 设置识别阈值 */
    STREAM_MSG_FACE_THRESHOLD_ACK = 27, /* 阈值确认 */
} stream_message_type_t;

/* 帧头结构体（大端序） */
typedef struct {
    uint32_t magic;                 /* 魔数 S31V */
    uint8_t version;                /* 协议版本 */
    uint8_t type;                   /* 消息类型 */
    uint16_t flags;                 /* 标志位 */
    uint32_t sequence;              /* 帧序号 */
    uint64_t timestamp_us;          /* 时间戳（微秒） */
    uint16_t width;                 /* 图像宽度 */
    uint16_t height;                /* 图像高度 */
    uint32_t payload_length;        /* 负载长度 */
    uint32_t header_crc32;          /* 帧头 CRC32 校验 */
} stream_frame_header_t;

/* 验证帧头有效性 */
esp_err_t stream_protocol_validate_header(const stream_frame_header_t *header, size_t max_payload);
/* 编码帧头为大端序字节数组 */
esp_err_t stream_protocol_encode_header(const stream_frame_header_t *header,
                                        uint8_t output[STREAM_PROTOCOL_HEADER_SIZE]);
/* 从字节数组解码帧头 */
esp_err_t stream_protocol_decode_header(const uint8_t input[STREAM_PROTOCOL_HEADER_SIZE],
                                        stream_frame_header_t *header, size_t max_payload);
/* 从 socket 读取帧头（含魔数同步和超时处理） */
esp_err_t stream_protocol_read_header(int socket, stream_frame_header_t *header,
                                      size_t max_payload, int timeout_ms);