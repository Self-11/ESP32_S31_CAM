/* 人脸检测服务实现
 * 基于 ESP-DL 的 MSRMNP（Multi-Scale RetinaFace + MobileNet-Prune）模型，
 * 在独立的 FreeRTOS 任务中异步执行推理，支持双缓冲无阻塞帧提交。
 * 模型输入：224x224 RGB565，输出：最多 4 个人脸框 + 5 点关键点。
 */

#include "face_detection_service.h"

#include <algorithm>
#include <new>
#include <string.h>

#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"

namespace {

/* 模型固定输入尺寸 */
constexpr uint16_t kModelWidth = 224;
constexpr uint16_t kModelHeight = 224;
constexpr size_t kInputBytes = kModelWidth * kModelHeight * sizeof(uint16_t);
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 1;        /* 绑定到 CPU1，避免与 WiFi 冲突 */

const char *TAG = "face_detect";
SemaphoreHandle_t s_input_lock;            /* 保护输入缓冲区的互斥锁 */
SemaphoreHandle_t s_detector_mutex;        /* 保护检测器非线程安全访问 */
TaskHandle_t s_task;                        /* 检测任务句柄 */
HumanFaceDetect *s_detector;               /* ESP-DL 检测器实例 */
uint16_t *s_input_buffers[2];              /* 双缓冲输入帧 */
uint16_t *s_sync_input_buffer;             /* 同步检测专用缓冲区 */
int s_active_buffer = -1;                  /* 当前正在推理的缓冲区索引 */
int s_pending_buffer = -1;                 /* 待推理的缓冲区索引 */
uint16_t s_pending_source_width;
uint16_t s_pending_source_height;
uint32_t s_pending_source_sequence;
portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;
face_detection_results_t s_latest;         /* 最新检测结果 */
volatile bool s_enabled = true;
volatile bool s_ready;                      /* 模型加载完成标志 */
volatile uint32_t s_inference_ms;           /* 最近推理耗时 */
volatile uint32_t s_dropped_frames;         /* 丢弃帧计数 */

/* 钳位无符号 16 位整数到 [0, maximum] 范围内 */
static uint16_t clamp_u16(int value, int maximum)
{
    return static_cast<uint16_t>(std::max(0, std::min(value, maximum)));
}

/* 人脸检测后台任务（绑定 CPU1）：
 * 循环等待最新待处理帧，执行 MSRMNP 推理后将结果（框坐标 + 关键点）
 * 写入 s_latest，并通过 generation 字段通知上层消费者。
 * 双缓冲无锁提交：新帧到达时非阻塞覆盖 pending_buffer 索引。 */
static void detection_task(void *)
{
    ESP_LOGI(TAG, "loading MSR+MNP face/landmark model; PSRAM free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));

    s_detector = new (std::nothrow)
        HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "face model allocation failed; PSRAM largest=%u",
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        vTaskDelete(nullptr);
        return;
    }
    s_detector_mutex = xSemaphoreCreateMutex();
    if (s_detector_mutex == nullptr) {
        ESP_LOGE(TAG, "detector mutex creation failed");
        delete s_detector;
        s_detector = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "MSR+MNP face/landmark model ready");

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_enabled) {
            continue;
        }

        int buffer_index = -1;
        uint16_t source_width = 0;
        uint16_t source_height = 0;
        uint32_t source_sequence = 0;
        xSemaphoreTake(s_input_lock, portMAX_DELAY);
        if (s_pending_buffer >= 0) {
            buffer_index = s_pending_buffer;
            s_pending_buffer = -1;
            s_active_buffer = buffer_index;
            source_width = s_pending_source_width;
            source_height = s_pending_source_height;
            source_sequence = s_pending_source_sequence;
        }
        xSemaphoreGive(s_input_lock);
        if (buffer_index < 0) {
            continue;
        }

        dl::image::img_t image = {
            .data = s_input_buffers[buffer_index],
            .width = kModelWidth,
            .height = kModelHeight,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        const int64_t started = esp_timer_get_time();
        xSemaphoreTake(s_detector_mutex, portMAX_DELAY);
        auto &detections = s_detector->run(image);
        xSemaphoreGive(s_detector_mutex);
        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);

        /* 模型返回的坐标是 224x224 归一化空间的，需要映射回原始图像坐标。
         * 先计算源图像的中心正方形裁剪区域，再将模型输出按比例缩放。 */
        face_detection_results_t next = {};
        next.source_sequence = source_sequence;
        next.source_width = source_width;
        next.source_height = source_height;
        const int crop_size = std::min<int>(source_width, source_height);
        const int crop_x = (source_width - crop_size) / 2;   /* 水平居中裁剪起点 */
        const int crop_y = (source_height - crop_size) / 2;  /* 垂直居中裁剪起点 */
        for (const auto &item : detections) {
            if (next.count >= FACE_DETECTION_MAX_RESULTS || item.box.size() < 4) {
                break;
            }
            /* 裁剪区域坐标 → 原图坐标变换 */
            const int left = crop_x + item.box[0] * crop_size / kModelWidth;
            const int top = crop_y + item.box[1] * crop_size / kModelHeight;
            const int right = crop_x + item.box[2] * crop_size / kModelWidth;
            const int bottom = crop_y + item.box[3] * crop_size / kModelHeight;
            face_detection_box_t &box = next.boxes[next.count++];
            box.x = clamp_u16(left, source_width - 1);
            box.y = clamp_u16(top, source_height - 1);
            const uint16_t right_clamped = clamp_u16(right, source_width - 1);
            const uint16_t bottom_clamped = clamp_u16(bottom, source_height - 1);
            box.width = std::max<uint16_t>(1, right_clamped - box.x);
            box.height = std::max<uint16_t>(1, bottom_clamped - box.y);
            box.score_x10000 = clamp_u16(static_cast<int>(item.score * 10000.0f), 10000);
            /* 5 点关键点坐标也从模型空间映射到原图坐标
             * 关键点顺序：左眼、右眼、鼻尖、左嘴角、右嘴角 (x,y) 交替 */
            if (item.keypoint.size() >= 10) {
                for (int k = 0; k < 5; ++k) {
                    box.keypoint[2 * k] = static_cast<int16_t>(
                        crop_x + item.keypoint[2 * k] * crop_size / kModelWidth);
                    box.keypoint[2 * k + 1] = static_cast<int16_t>(
                        crop_y + item.keypoint[2 * k + 1] * crop_size / kModelHeight);
                }
            } else {
                memset(box.keypoint, 0, sizeof(box.keypoint));
            }
        }

        taskENTER_CRITICAL(&s_result_lock);
        next.generation = s_latest.generation + 1;
        s_latest = next;
        s_inference_ms = elapsed_ms;
        taskEXIT_CRITICAL(&s_result_lock);

        /* 当帧处理完成、发布结果后，检查是否有积压帧需要立即处理。
         * 核心里程碑：双缓冲流水线的继续条件——如果 pending ≥ 0，
         * 给自己发通知立即开始下一轮处理，实现零间隙连续检测。 */
        xSemaphoreTake(s_input_lock, portMAX_DELAY);
        s_active_buffer = -1;
        const bool more_pending = s_pending_buffer >= 0;
        xSemaphoreGive(s_input_lock);
        if (more_pending) {
            xTaskNotifyGive(s_task);
        }
    }
}

} // namespace

/* 初始化检测服务：创建互斥锁、分配双缓冲和同步输入缓冲、启动检测任务 */
extern "C" esp_err_t face_detection_service_init(void)
{
    if (s_task != nullptr) {
        return ESP_OK;
    }
    s_input_lock = xSemaphoreCreateMutex();
    if (s_input_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_sync_input_buffer = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        64, kInputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (s_sync_input_buffer == nullptr) {
        ESP_LOGE(TAG, "sync input allocation failed; largest PSRAM=%u",
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }
    for (auto &buffer : s_input_buffers) {
        buffer = static_cast<uint16_t *>(heap_caps_aligned_alloc(
            64, kInputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
        if (buffer == nullptr) {
            ESP_LOGE(TAG, "224x224 input allocation failed; largest PSRAM=%u",
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreatePinnedToCore(detection_task, "face_detection", kTaskStackBytes, nullptr,
                                kTaskPriority, &s_task, kTaskCore) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 设置检测启用标志，禁用时清空待处理缓冲和最新结果 */
extern "C" esp_err_t face_detection_service_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled && s_input_lock != nullptr) {
        xSemaphoreTake(s_input_lock, portMAX_DELAY);
        s_pending_buffer = -1;
        xSemaphoreGive(s_input_lock);
        taskENTER_CRITICAL(&s_result_lock);
        s_latest.count = 0;
        ++s_latest.generation;
        taskEXIT_CRITICAL(&s_result_lock);
    }
    return ESP_OK;
}

extern "C" bool face_detection_service_is_enabled(void) { return s_enabled; }
extern "C" bool face_detection_service_is_ready(void) { return s_ready; }

/* 提交 RGB565 帧到异步检测队列。
 * 双缓冲无锁提交：新帧到达时非阻塞覆盖 pending_buffer 索引。
 * 若互斥锁不可用（被检测任务持有），丢弃本帧并递增丢弃计数。 */
extern "C" esp_err_t face_detection_service_submit_rgb565(const uint16_t *pixels,
                                                            uint16_t width,
                                                            uint16_t height,
                                                            uint32_t source_sequence)
{
    if (!s_enabled || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == nullptr || width == 0 || height == 0 || s_input_lock == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_input_lock, 0) != pdTRUE) {
        s_dropped_frames = s_dropped_frames + 1U;
        return ESP_ERR_TIMEOUT;
    }
    int target = 0;
    if (s_active_buffer == 0) {
        target = 1;
    } else if (s_active_buffer == 1) {
        target = 0;
    } else if (s_pending_buffer == 0) {
        target = 0;
    } else if (s_pending_buffer == 1) {
        target = 1;
    }

    /* 将源图像中心裁剪后降采样到 224x224 模型输入尺寸。
     * 双线性缩小：crop_size / kModelHeight/Width 步长 */
    const uint32_t crop_size = std::min<uint16_t>(width, height);
    const uint32_t crop_x = (width - crop_size) / 2U;
    const uint32_t crop_y = (height - crop_size) / 2U;
    uint16_t *destination = s_input_buffers[target];
    for (uint32_t y = 0; y < kModelHeight; ++y) {
        const uint32_t source_y = crop_y + y * crop_size / kModelHeight;
        for (uint32_t x = 0; x < kModelWidth; ++x) {
            const uint32_t source_x = crop_x + x * crop_size / kModelWidth;
            destination[y * kModelWidth + x] = pixels[source_y * width + source_x];
        }
    }
    /* 如果已有待处理帧未处理，丢弃之（更新 pending 覆盖），计为丢弃帧 */
    if (s_pending_buffer >= 0) {
        s_dropped_frames = s_dropped_frames + 1U;
    }
    s_pending_buffer = target;
    s_pending_source_width = width;
    s_pending_source_height = height;
    s_pending_source_sequence = source_sequence;
    xSemaphoreGive(s_input_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

/* 获取最新异步检测结果（线程安全快照拷贝） */
extern "C" bool face_detection_service_get_latest(face_detection_results_t *results)
{
    if (results == nullptr) {
        return false;
    }
    taskENTER_CRITICAL(&s_result_lock);
    *results = s_latest;
    taskEXIT_CRITICAL(&s_result_lock);
    return s_ready;
}

extern "C" uint32_t face_detection_service_inference_ms(void) { return s_inference_ms; }
extern "C" uint32_t face_detection_service_dropped_frames(void) { return s_dropped_frames; }

/* 同步人脸检测（调用方线程阻塞等待 MSRMNP 推理完成）。
 * 与异步流水线共享同一个检测器，通过互斥锁串行化。
 * 主要用于人脸注册场景，需要立即获取精确的关键点坐标。 */
extern "C" esp_err_t face_detection_service_detect_sync(const uint16_t *pixels,
                                                          uint16_t width, uint16_t height,
                                                          face_detection_results_t *out)
{
    if (!s_ready || s_detector == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == nullptr || width == 0 || height == 0 || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sync_input_buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    memset(out, 0, sizeof(*out));
    out->source_width = width;
    out->source_height = height;

    const uint32_t crop_size = std::min<uint16_t>(width, height);
    const uint32_t crop_x = (width - crop_size) / 2U;
    const uint32_t crop_y = (height - crop_size) / 2U;

    for (uint32_t y = 0; y < kModelHeight; ++y) {
        const uint32_t sy = crop_y + y * crop_size / kModelHeight;
        for (uint32_t x = 0; x < kModelWidth; ++x) {
            const uint32_t sx = crop_x + x * crop_size / kModelWidth;
            s_sync_input_buffer[y * kModelWidth + x] = pixels[sy * width + sx];
        }
    }

    dl::image::img_t image = {
        .data = s_sync_input_buffer,
        .width = kModelWidth,
        .height = kModelHeight,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    xSemaphoreTake(s_detector_mutex, portMAX_DELAY);
    auto &detections = s_detector->run(image);
    for (const auto &item : detections) {
        if (out->count >= FACE_DETECTION_MAX_RESULTS || item.box.size() < 4) {
            break;
        }
        const int left = crop_x + item.box[0] * crop_size / kModelWidth;
        const int top = crop_y + item.box[1] * crop_size / kModelHeight;
        const int right = crop_x + item.box[2] * crop_size / kModelWidth;
        const int bottom = crop_y + item.box[3] * crop_size / kModelHeight;
        face_detection_box_t &box = out->boxes[out->count++];
        box.x = clamp_u16(left, width - 1);
        box.y = clamp_u16(top, height - 1);
        const uint16_t rc = clamp_u16(right, width - 1);
        const uint16_t bc = clamp_u16(bottom, height - 1);
        box.width = std::max<uint16_t>(1, rc - box.x);
        box.height = std::max<uint16_t>(1, bc - box.y);
        box.score_x10000 = clamp_u16(static_cast<int>(item.score * 10000.0f), 10000);
        if (item.keypoint.size() >= 10) {
            for (int k = 0; k < 5; ++k) {
                box.keypoint[2 * k] = static_cast<int16_t>(
                    crop_x + item.keypoint[2 * k] * crop_size / kModelWidth);
                box.keypoint[2 * k + 1] = static_cast<int16_t>(
                    crop_y + item.keypoint[2 * k + 1] * crop_size / kModelHeight);
            }
        } else {
            memset(box.keypoint, 0, sizeof(box.keypoint));
        }
    }
    xSemaphoreGive(s_detector_mutex);

    return (out->count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}