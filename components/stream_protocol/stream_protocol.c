/* 流传输协议实现
 * 提供帧头的大端序编解码、CRC32 校验、以及从 socket 读取帧头（含魔数同步）的功能。
 */

#include "stream_protocol.h"

#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_crc.h"

/* 大端序写入辅助函数 */
static void write_u16_be(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8);
    target[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *target, uint64_t value)
{
    write_u32_be(target, (uint32_t)(value >> 32));
    write_u32_be(target + 4, (uint32_t)value);
}

/* 大端序读取辅助函数 */
static uint16_t read_u16_be(const uint8_t *source)
{
    return ((uint16_t)source[0] << 8) | source[1];
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) | source[3];
}

static uint64_t read_u64_be(const uint8_t *source)
{
    return ((uint64_t)read_u32_be(source) << 32) | read_u32_be(source + 4);
}

/* 验证帧头的魔数、版本、类型和负载长度 */
esp_err_t stream_protocol_validate_header(const stream_frame_header_t *header, size_t max_payload)
{
    if (header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (header->magic != STREAM_PROTOCOL_MAGIC || header->version != STREAM_PROTOCOL_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (header->type < STREAM_MSG_HELLO || header->type > STREAM_MSG_FACE_THRESHOLD_ACK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (header->payload_length > max_payload) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* 将帧头编码为大端序 32 字节数组，包含 CRC32 校验 */
esp_err_t stream_protocol_encode_header(const stream_frame_header_t *header,
                                        uint8_t output[STREAM_PROTOCOL_HEADER_SIZE])
{
    if (header == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = stream_protocol_validate_header(header, UINT32_MAX);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(output, 0, STREAM_PROTOCOL_HEADER_SIZE);
    write_u32_be(output, header->magic);
    output[4] = header->version;
    output[5] = header->type;
    write_u16_be(output + 6, header->flags);
    write_u32_be(output + 8, header->sequence);
    write_u64_be(output + 12, header->timestamp_us);
    write_u16_be(output + 20, header->width);
    write_u16_be(output + 22, header->height);
    write_u32_be(output + 24, header->payload_length);
    /* CRC32 计算覆盖前 28 字节，写入位置 28-31 */
    write_u32_be(output + 28, esp_crc32_be(UINT32_MAX, output, 28));
    return ESP_OK;
}

/* 从字节数组解码帧头，同时校验 CRC32 */
esp_err_t stream_protocol_decode_header(const uint8_t input[STREAM_PROTOCOL_HEADER_SIZE],
                                        stream_frame_header_t *header, size_t max_payload)
{
    if (input == NULL || header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t expected_crc = esp_crc32_be(UINT32_MAX, input, 28);
    if (read_u32_be(input + 28) != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    *header = (stream_frame_header_t) {
        .magic = read_u32_be(input),
        .version = input[4],
        .type = input[5],
        .flags = read_u16_be(input + 6),
        .sequence = read_u32_be(input + 8),
        .timestamp_us = read_u64_be(input + 12),
        .width = read_u16_be(input + 20),
        .height = read_u16_be(input + 22),
        .payload_length = read_u32_be(input + 24),
        .header_crc32 = read_u32_be(input + 28),
    };
    return stream_protocol_validate_header(header, max_payload);
}

/* 从 socket 读取帧头，使用滑动窗口同步魔数 0x53333156 */
esp_err_t stream_protocol_read_header(int socket, stream_frame_header_t *header,
                                      size_t max_payload, int timeout_ms)
{
    if (header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(header, 0, sizeof(*header));

    uint8_t header_buf[STREAM_PROTOCOL_HEADER_SIZE];
    uint32_t magic_state = 0;           /* 32 位滑动窗口用于魔数同步 */

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };
    struct timeval default_tv = tv;

    /* 逐字节读取直到匹配魔数 */
    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        tv = default_tv;
        if (select(socket + 1, &readfds, NULL, NULL, &tv) <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        uint8_t byte;
        ssize_t n = recv(socket, &byte, 1, 0);
        if (n != 1) {
            return ESP_FAIL;
        }

        magic_state = (magic_state << 8) | byte;  /* 滑动窗口左移 */
        if (magic_state == STREAM_PROTOCOL_MAGIC) {
            /* 匹配到魔数，将已读取的 4 字节存入缓冲区 */
            header_buf[0] = (uint8_t)(magic_state >> 24);
            header_buf[1] = (uint8_t)(magic_state >> 16);
            header_buf[2] = (uint8_t)(magic_state >> 8);
            header_buf[3] = (uint8_t)(magic_state);

            /* 读取剩余的 28 字节 */
            size_t remaining = STREAM_PROTOCOL_HEADER_SIZE - 4;
            size_t received = 0;
            while (received < remaining) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(socket, &rfds);
                tv = default_tv;
                if (select(socket + 1, &rfds, NULL, NULL, &tv) <= 0) {
                    return ESP_ERR_TIMEOUT;
                }
                ssize_t n2 = recv(socket, header_buf + 4 + received, remaining - received, 0);
                if (n2 <= 0) {
                    return ESP_FAIL;
                }
                received += (size_t)n2;
            }

            esp_err_t ret = stream_protocol_decode_header(header_buf, header, max_payload);
            if (ret == ESP_OK) {
                return ESP_OK;
            }
            /* CRC 无效，重置魔数状态继续搜索 */
            magic_state = 0;
        }
    }
}