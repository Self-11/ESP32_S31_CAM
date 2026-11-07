/* SD 卡录像服务实现
 * 将 JPEG 帧封装为 AVI 容器写入 microSD 卡，格式为 MJPEG 视频。
 * 功能特性：
 * - 按时间分段（默认 300 秒）自动创建新文件
 * - 磁盘空间不足时自动删除最旧的录像文件
 * - 支持运行时分段文件名基于时间戳生成
 * - AVI 索引文件（.record.idx）用于未完成录像的恢复
 */

#include "recording_service.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_service.h"
#include "media_service.h"
#include "storage_service.h"

#define RECORDING_TASK_STACK       6144U
#define RECORDING_TASK_PRIORITY    3U
#define RECORDING_SEGMENT_SECONDS  300U
#define RECORDING_MIN_FREE_BYTES   (256ULL * 1024ULL * 1024ULL)
#define RECORDING_DIR              "/sdcard/recordings"

typedef struct {
    FILE *avi;
    FILE *index;
    char part_path[160];
    char final_path[128];
    char index_path[128];
    long riff_size_pos;
    long main_header_pos;
    long stream_header_pos;
    long movi_size_pos;
    long movi_offset_base;
    uint32_t frames;
    uint32_t max_frame;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    time_t opened_at;
    bool opened_with_valid_time;
} avi_writer_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} avi_index_entry_t;

static const char *TAG = "recording_service";
static volatile bool s_running;
static TaskHandle_t s_task;
static uint32_t s_total_frames;
static uint32_t s_file_sequence;
static char s_current_file[128];

/* 判断系统时间是否有效（须在 2024-01-01 之后） */
static bool wall_clock_valid(time_t value)
{
    return value >= 1704067200; /* 2024-01-01 00:00:00 UTC */
}

/* 生成 AVI 最终文件路径（基于时间戳 + 文件序号递增） */
static void make_final_path(char *path, size_t size, time_t value)
{
    struct tm local = {0};
    localtime_r(&value, &local);
    ++s_file_sequence;
    snprintf(path, size,
             RECORDING_DIR "/REC_%04d%02d%02d_%02d%02d%02d_%03lu.avi",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec,
             (unsigned long)s_file_sequence);
}

/* 写入固定大小字节块，返回是否完整写入 */
static bool write_bytes(FILE *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, file) == size;
}

/* 写入小端 uint16 */
static bool write_u16(FILE *file, uint16_t value)
{
    uint8_t data[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return write_bytes(file, data, sizeof(data));
}

/* 写入小端 uint32 */
static bool write_u32(FILE *file, uint32_t value)
{
    uint8_t data[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    return write_bytes(file, data, sizeof(data));
}

/* 定位到指定偏移写入 uint32 并恢复原文件指针（用于回填 AVI 头大小字段） */
static bool patch_u32(FILE *file, long position, uint32_t value)
{
    long current = ftell(file);
    return current >= 0 && fseek(file, position, SEEK_SET) == 0 &&
           write_u32(file, value) && fseek(file, current, SEEK_SET) == 0;
}

/* 回填一个 LIST chunk 的大小字段（在写完子内容后被调用） */
static bool finish_list(FILE *file, long size_position)
{
    long end = ftell(file);
    return end >= 0 && patch_u32(file, size_position,
                                 (uint32_t)(end - size_position - 4));
}

/* 写入一个 RIFF / AVI chunk：[4B tag][4B size][data...][optional padding]
 * 返回当前 chunk 数据部分的文件偏移（用于后续 patch）。 */
static bool write_chunk(FILE *file, const char tag[4], const void *data, uint32_t size,
                        long *position)
{
    if (position != NULL) {
        *position = ftell(file);
    }
    return write_bytes(file, tag, 4) && write_u32(file, size) &&
           write_bytes(file, data, size) && (!(size & 1U) || fputc(0, file) != EOF);
}

/* SD 卡空间不足时自动删除最旧的 .avi 文件（保留至少 256MB 空闲空间） */
static void remove_oldest_if_needed(void)
{
    struct statvfs fs;
    /* 循环删除最旧的 .avi 文件，直到剩余空间 > RECORDING_MIN_FREE_BYTES (256MB) */
    while (statvfs(RECORDING_DIR, &fs) == 0 &&
           (uint64_t)fs.f_bavail * fs.f_frsize < RECORDING_MIN_FREE_BYTES) {
        DIR *directory = opendir(RECORDING_DIR);
        if (directory == NULL) {
            return;
        }
        char oldest[128] = {0};
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len < 5 || strcmp(entry->d_name + len - 4, ".avi") != 0) {
                continue;
            }
            if (oldest[0] == '\0' || strcmp(entry->d_name, oldest) < 0) {
                strlcpy(oldest, entry->d_name, sizeof(oldest));
            }
        }
        closedir(directory);
        if (oldest[0] == '\0') {
            return;
        }
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", RECORDING_DIR, oldest);
        if (unlink(path) != 0) {
            ESP_LOGE(TAG, "cannot remove old recording %s: errno=%d", path, errno);
            return;
        }
        ESP_LOGW(TAG, "low space: removed oldest recording %s", path);
    }
}

/* 打开 AVI 文件并写入标准 AVI 文件头。
 * AVI 容器结构（简化）：
 *   RIFF
 *    ├── LIST hdrl
 *    │    ├── avih (主帧头: 分辨率/FPS/帧数/最大帧尺寸)
 *    │    └── LIST strl
 *    │         ├── strh (流帧头: MJPG 编码)
 *    │         └── strf (流格式: 分辨率/位深度)
 *    └── LIST movi
 *          ├── 00dc (帧1 JPEG数据)
 *          ├── 00dc (帧2)
 *          └── ...
 * 写入时预留占位符，avi_close 时回填实际值 */
static bool avi_open(avi_writer_t *writer, uint16_t width, uint16_t height, uint16_t fps)
{
    memset(writer, 0, sizeof(*writer));
    remove_oldest_if_needed();
    time_t now = time(NULL);
    make_final_path(writer->final_path, sizeof(writer->final_path), now);
    snprintf(writer->part_path, sizeof(writer->part_path), "%s.part", writer->final_path);
    snprintf(writer->index_path, sizeof(writer->index_path), RECORDING_DIR "/.record.idx");
    writer->avi = fopen(writer->part_path, "w+b");
    writer->index = fopen(writer->index_path, "w+b");
    if (writer->avi == NULL || writer->index == NULL) {
        ESP_LOGE(TAG, "open recording failed: errno=%d", errno);
        if (writer->avi) fclose(writer->avi);
        if (writer->index) fclose(writer->index);
        memset(writer, 0, sizeof(*writer));
        return false;
    }
    writer->width = width;
    writer->height = height;
    writer->fps = fps;
    writer->opened_at = now;
    writer->opened_with_valid_time = wall_clock_valid(now);

    FILE *f = writer->avi;
    bool ok = write_bytes(f, "RIFF", 4);
    writer->riff_size_pos = ftell(f);
    ok &= write_u32(f, 0) && write_bytes(f, "AVI ", 4) && write_bytes(f, "LIST", 4);
    long hdrl_size = ftell(f);
    ok &= write_u32(f, 0) && write_bytes(f, "hdrl", 4);

    uint8_t avih[56] = {0};
    long avih_chunk;
    ok &= write_chunk(f, "avih", avih, sizeof(avih), &avih_chunk);
    writer->main_header_pos = avih_chunk + 8;
    ok &= patch_u32(f, writer->main_header_pos, 1000000U / fps);
    ok &= patch_u32(f, writer->main_header_pos + 12, 0x10);
    ok &= patch_u32(f, writer->main_header_pos + 24, 1);
    ok &= patch_u32(f, writer->main_header_pos + 32, width);
    ok &= patch_u32(f, writer->main_header_pos + 36, height);

    ok &= write_bytes(f, "LIST", 4);
    long strl_size = ftell(f);
    ok &= write_u32(f, 0) && write_bytes(f, "strl", 4);
    uint8_t strh[56] = {0};
    memcpy(strh, "vidsMJPG", 8);
    long strh_chunk;
    ok &= write_chunk(f, "strh", strh, sizeof(strh), &strh_chunk);
    writer->stream_header_pos = strh_chunk + 8;
    ok &= patch_u32(f, writer->stream_header_pos + 20, 1);
    ok &= patch_u32(f, writer->stream_header_pos + 24, fps);
    ok &= patch_u32(f, writer->stream_header_pos + 40, UINT32_MAX);
    long current = ftell(f);
    ok &= fseek(f, writer->stream_header_pos + 52, SEEK_SET) == 0;
    ok &= write_u16(f, width) && write_u16(f, height);
    ok &= fseek(f, current, SEEK_SET) == 0;

    uint8_t strf[40] = {0};
    strf[0] = 40;
    memcpy(strf + 16, "MJPG", 4);
    long strf_chunk;
    ok &= write_chunk(f, "strf", strf, sizeof(strf), &strf_chunk);
    ok &= patch_u32(f, strf_chunk + 8 + 4, width);
    ok &= patch_u32(f, strf_chunk + 8 + 8, height);
    current = ftell(f);
    ok &= fseek(f, strf_chunk + 8 + 12, SEEK_SET) == 0;
    ok &= write_u16(f, 1) && write_u16(f, 24);
    ok &= fseek(f, current, SEEK_SET) == 0;
    ok &= patch_u32(f, strf_chunk + 8 + 20, (uint32_t)width * height * 3U);
    ok &= finish_list(f, strl_size) && finish_list(f, hdrl_size);
    ok &= write_bytes(f, "LIST", 4);
    writer->movi_size_pos = ftell(f);
    ok &= write_u32(f, 0) && write_bytes(f, "movi", 4);
    writer->movi_offset_base = writer->movi_size_pos + 4;
    if (!ok) {
        ESP_LOGE(TAG, "AVI header write failed");
        fclose(writer->avi);
        fclose(writer->index);
        writer->avi = NULL;
        writer->index = NULL;
        unlink(writer->part_path);
        unlink(writer->index_path);
        return false;
    }
    strlcpy(s_current_file, writer->final_path, sizeof(s_current_file));
    ESP_LOGI(TAG, "recording started: %s (%ux%u @ %u fps)",
             writer->final_path, width, height, fps);
    return true;
}

/* 向 AVI 文件追加一帧 JPEG 数据（"00dc" chunk）。
 * 硬件编码器可能在 JPEG EOI 后残留 DMA 对齐填充字节，
 * 因此反向搜索实际的 EOF 位置截断。
 * 每 FPS 帧刷写一次 SD 卡以确保数据安全。 */
static bool avi_add_frame(avi_writer_t *writer, const jpeg_frame_t *frame)
{
    if (frame->size < 4 || frame->data[0] != 0xFF || frame->data[1] != 0xD8) {
        ESP_LOGE(TAG, "invalid JPEG SOI: size=%u", (unsigned)frame->size);
        return false;
    }
    /* 反向搜索 JPEG EOI (FF D9)，截断 DMA 填充字节 */
    size_t jpeg_size = frame->size;
    while (jpeg_size >= 2 &&
           !(frame->data[jpeg_size - 2] == 0xFF && frame->data[jpeg_size - 1] == 0xD9)) {
        --jpeg_size;
    }
    if (jpeg_size < 2) {
        ESP_LOGE(TAG, "invalid JPEG EOI: size=%u", (unsigned)frame->size);
        return false;
    }
    long position;
    if (!write_chunk(writer->avi, "00dc", frame->data, (uint32_t)jpeg_size, &position)) {
        ESP_LOGE(TAG, "AVI frame write failed: errno=%d", errno);
        return false;
    }
    avi_index_entry_t entry = {
        .offset = (uint32_t)(position - writer->movi_offset_base),
        .size = (uint32_t)jpeg_size,
    };
    if (!write_bytes(writer->index, &entry, sizeof(entry))) {
        ESP_LOGE(TAG, "AVI index write failed: errno=%d", errno);
        return false;
    }
    ++writer->frames;
    if (jpeg_size > writer->max_frame) writer->max_frame = jpeg_size;
    if ((writer->frames % writer->fps) == 0U) {
        if (fflush(writer->avi) != 0 || fflush(writer->index) != 0) {
            ESP_LOGE(TAG, "periodic SD flush failed: errno=%d", errno);
            return false;
        }
    }
    return true;
}

/* 关闭 AVI 文件，回填帧头中的实际帧数、最大帧尺寸等动态字段，
 * 写入 idx1 索引块，计算并回填 RIFF 总大小。
 * 完成后将 .part 临时文件重命名为 .avi 正式文件名。 */
static void avi_close(avi_writer_t *writer)
{
    if (writer->avi == NULL) return;
    long index_position = ftell(writer->avi);
    bool ok = index_position >= 0 && patch_u32(writer->avi, writer->movi_size_pos,
                                               (uint32_t)(index_position - writer->movi_size_pos - 4));
    ok &= write_bytes(writer->avi, "idx1", 4) && write_u32(writer->avi, writer->frames * 16U);
    rewind(writer->index);
    avi_index_entry_t entry;
    while (ok && fread(&entry, sizeof(entry), 1, writer->index) == 1) {
        ok &= write_bytes(writer->avi, "00dc", 4) && write_u32(writer->avi, 0x10) &&
              write_u32(writer->avi, entry.offset) && write_u32(writer->avi, entry.size);
    }
    ok &= patch_u32(writer->avi, writer->main_header_pos + 4,
                    writer->max_frame * writer->fps);
    ok &= patch_u32(writer->avi, writer->main_header_pos + 16, writer->frames);
    ok &= patch_u32(writer->avi, writer->main_header_pos + 28, writer->max_frame);
    ok &= patch_u32(writer->avi, writer->stream_header_pos + 32, writer->frames);
    ok &= patch_u32(writer->avi, writer->stream_header_pos + 36, writer->max_frame);
    long end = ftell(writer->avi);
    ok &= end >= 0 && patch_u32(writer->avi, writer->riff_size_pos, (uint32_t)(end - 8));
    ok &= fflush(writer->avi) == 0;
    ok &= fclose(writer->avi) == 0;
    ok &= fclose(writer->index) == 0;
    writer->avi = NULL;
    writer->index = NULL;
    unlink(writer->index_path);
    time_t now = time(NULL);
    if (!writer->opened_with_valid_time && wall_clock_valid(now)) {
        make_final_path(writer->final_path, sizeof(writer->final_path), now);
    }
    if (ok && writer->frames > 0 && rename(writer->part_path, writer->final_path) == 0) {
        ESP_LOGI(TAG, "recording finalized: %s, %lu frames", writer->final_path,
                 (unsigned long)writer->frames);
    } else {
        ESP_LOGE(TAG, "recording finalization failed: %s, frames=%lu errno=%d",
                 writer->part_path, (unsigned long)writer->frames, errno);
        if (writer->frames == 0) {
            unlink(writer->part_path);
        }
    }
    s_current_file[0] = '\0';
}

/* 录制后台任务主循环：
 * 不断获取最新 JPEG 帧写入 AVI 文件，根据分辨率变化、300 秒
 * 分段或时间戳有效性变化自动轮换文件。240p 及以下用 24fps，480p 用 10fps。 */
static void recording_task(void *argument)
{
    (void)argument;
    avi_writer_t writer = {0};
    uint32_t last_sequence = 0;

    /* 主录制循环：获取最新 JPEG 帧 → 写入 AVI 文件
     * - 分辨率变化、300 秒分段、或时间戳有效性变化时自动轮换文件
     * - 240p 及以下分辨率使用 24fps，480p 使用 10fps */
    while (s_running) {
        jpeg_frame_t frame = {._slot = UINT8_MAX};
        if (!jpeg_service_acquire_latest(&frame) || frame.sequence == last_sequence) {
            if (frame._slot != UINT8_MAX) jpeg_service_release(&frame);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_sequence = frame.sequence;
        const uint16_t fps = frame.width <= 240 ? 24 : 10;
        time_t now = time(NULL);

        /* 判断是否需要轮换文件 */
        bool rotate = writer.avi != NULL &&
                      (frame.width != writer.width || frame.height != writer.height ||
                       now - writer.opened_at >= RECORDING_SEGMENT_SECONDS ||
                       (!writer.opened_with_valid_time && wall_clock_valid(now)));
        if (rotate) avi_close(&writer);
        if (writer.avi == NULL && !avi_open(&writer, frame.width, frame.height, fps)) {
            jpeg_service_release(&frame);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!avi_add_frame(&writer, &frame)) {
            ESP_LOGE(TAG, "SD write failed; recorder stopped until reboot");
            avi_close(&writer);
            jpeg_service_release(&frame);
            s_running = false;
            break;
        } else {
            ++s_total_frames;
        }
        jpeg_service_release(&frame);
    }
    avi_close(&writer);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* 启动 SD 卡录制后台任务 */
esp_err_t recording_service_start(void)
{
    if (s_running) return ESP_OK;
    if (!storage_service_sd_mounted()) return ESP_ERR_INVALID_STATE;
    if (mkdir(RECORDING_DIR, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    s_running = true;
    if (xTaskCreate(recording_task, "sd_recorder", RECORDING_TASK_STACK, NULL,
                    RECORDING_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 停止 SD 卡录制，等待录制任务最多 3 秒完成 */
esp_err_t recording_service_stop(void)
{
    s_running = false;
    for (uint32_t waited = 0; s_task != NULL && waited < 3000; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_task == NULL ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool recording_service_is_running(void) { return s_running; }
uint32_t recording_service_frame_count(void) { return s_total_frames; }
const char *recording_service_current_file(void) { return s_current_file; }