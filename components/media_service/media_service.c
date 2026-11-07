/* 媒体服务实现
 * 摄像头帧采集与处理流水线的核心实现：
 * - 使用 ESP Video Device (V4L2) 驱动 DVP 摄像头
 * - 采集 RGB565 帧后双路输出：LVGL 屏幕预览 + JPEG 硬件编码
 * - 间隔若干帧（FACE_DETECTION_INTERVAL）异步提交人脸检测
 * - 在人脸检测回调中执行人脸识别（基于关键点对齐）
 * - 维护人脸识别确认状态机（需连续 N 帧匹配才确认）
 */

#include "media_service.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "board_service.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "face_detection_service.h"
#include "face_recognition_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_service.h"
#include "linux/videodev2.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PREVIEW_MAX_WIDTH          640U
#define PREVIEW_MAX_HEIGHT         480U
#define PREVIEW_BUFFER_COUNT       2U
#define PREVIEW_MAX_FRAME_BYTES    (PREVIEW_MAX_WIDTH * PREVIEW_MAX_HEIGHT * sizeof(uint16_t))
#define CAPTURE_TASK_STACK_BYTES   4096U
#define CAPTURE_TASK_PRIORITY      5U
#define CAPTURE_TASK_CORE          1U
#define DQBUF_TIMEOUT_MS           200U
#define STOP_TIMEOUT_MS            1000U
#define JPEG_QUALITY               80U
#define FACE_DETECTION_INTERVAL    5U
#define FACE_RECOGNITION_LABEL_LEN 48U
#define FACE_NAME_MAX             32U
#define FACE_NAME_LEN             24U
#define FACE_CONFIRM_MATCHES       2U
#define FACE_HOLD_TIME_US          1500000LL

static const char *TAG = "media_service";

typedef struct {
    void *address;
    size_t length;
} mapped_buffer_t;

typedef struct {
    uint16_t id;
    float confidence;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    bool valid;
} face_recognition_label_t;

typedef struct {
    uint16_t pending_id;
    uint8_t pending_matches;
    uint16_t confirmed_id;
    float confirmed_confidence;
    int64_t last_confirmed_match_us;
} face_recognition_state_t;

static bool s_initialized;
static volatile bool s_running;
static int s_fd = -1;
static TaskHandle_t s_capture_task;
static SemaphoreHandle_t s_mode_lock;
static mapped_buffer_t s_capture_buffers[PREVIEW_BUFFER_COUNT];
static uint16_t *s_canvas_buffer;
static lv_obj_t *s_canvas;
static lv_obj_t *s_face_overlay;
static lv_obj_t *s_status_label;
static volatile uint32_t s_frame_count;
static int64_t s_fps_window_start_us;
static uint32_t s_fps_window_frames;
static face_recognition_label_t s_recognition_labels[FACE_DETECTION_MAX_RESULTS];
static face_recognition_state_t s_recognition_states[FACE_DETECTION_MAX_RESULTS];
static char s_face_names[FACE_NAME_MAX][FACE_NAME_LEN];

static media_mode_t s_mode = {
    .width = PREVIEW_MAX_WIDTH,
    .height = PREVIEW_MAX_HEIGHT,
    .format = MEDIA_PIXEL_FORMAT_RGB565,
    .jpeg_quality = JPEG_QUALITY,
};

/* LVGL 人脸框叠加层绘制回调（LV_EVENT_DRAW_MAIN）：
 * 遍历所有有效的 recognition_labels，绘制：
 * - 矩形框：已识别绿色(0x22C55E)，未识别橙色(0xF59E0B)
 * - 标签文字：有姓名显示姓名+置信度，无姓名显示 ID+置信度，未知显示检测分数
 *
 * 注意：矩形和标签坐标都基于 overlay 对象的屏幕坐标偏移，
 * 不依赖 faces.boxes 的数组下标（解决异步错位问题）。 */
static void face_overlay_draw(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    if (layer == NULL) {
        return;
    }
    face_detection_results_t faces = {0};
    if (!face_detection_service_get_latest(&faces) ||
        faces.source_width != s_mode.width || faces.source_height != s_mode.height) {
        return;
    }
    lv_area_t overlay_coordinates;
    lv_obj_get_coords(lv_event_get_target_obj(event), &overlay_coordinates);

    lv_draw_rect_dsc_t rectangle;
    lv_draw_rect_dsc_init(&rectangle);
    rectangle.bg_opa = LV_OPA_TRANSP;
    rectangle.border_width = 3;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = &lv_font_montserrat_14;

    for (uint8_t i = 0; i < FACE_DETECTION_MAX_RESULTS; ++i) {
        if (!s_recognition_labels[i].valid) {
            continue;
        }
        const face_recognition_label_t *rl = &s_recognition_labels[i];
        const bool recognized = rl->id > 0;
        rectangle.border_color = lv_color_hex(recognized ? 0x22C55E : 0xF59E0B);
        lv_area_t area = {
            .x1 = overlay_coordinates.x1 + rl->x,
            .y1 = overlay_coordinates.y1 + rl->y,
            .x2 = overlay_coordinates.x1 + rl->x + rl->width,
            .y2 = overlay_coordinates.y1 + rl->y + rl->height,
        };
        lv_draw_rect(layer, &rectangle, &area);

        /* 名字查找优先级：
         * 1. 从 s_face_names 表中查找注册的人名（GPU 侧会通过 NVS 持久化）
         * 2. 如果有人名显示 "Name %.2f"，否则显示 "ID:%u %.2f"
         * 3. 未识别的人脸显示 "unknown det:%.2f" + 检测分数 */
        char text[FACE_RECOGNITION_LABEL_LEN];
        const char *name = "";
        if (rl->id > 0 && rl->id < FACE_NAME_MAX) {
            name = s_face_names[rl->id][0] ? s_face_names[rl->id] : "";
        }
        if (rl->id > 0 && name[0]) {
            snprintf(text, sizeof(text), "%s %.2f", name, (double)rl->confidence);
        } else if (rl->id > 0) {
            snprintf(text, sizeof(text), "ID:%u %.2f", rl->id, (double)rl->confidence);
        } else {
            const float detection_score = i < faces.count ?
                (float)faces.boxes[i].score_x10000 / 10000.0f : 0.0f;
            snprintf(text, sizeof(text), "unknown det:%.2f", (double)detection_score);
        }
        label_dsc.color = lv_color_hex(rl->id > 0 ? 0x22C55E : 0xF59E0B);
        lv_area_t label_area = {
            .x1 = overlay_coordinates.x1 + rl->x,
            .y1 = overlay_coordinates.y1 + (lv_coord_t)rl->y - 18,
            .x2 = overlay_coordinates.x1 + rl->x + 120,
            .y2 = overlay_coordinates.y1 + rl->y,
        };
        label_dsc.text = text;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

/* 重置所有 V4L2 采集缓冲区指针归零 */
static void reset_capture_buffers(void)
{
    for (size_t i = 0; i < PREVIEW_BUFFER_COUNT; ++i) {
        s_capture_buffers[i].address = NULL;
        s_capture_buffers[i].length = 0;
    }
}

/* 释放所有 V4L2 采集资源：停止流、解除内存映射、关闭设备 */
static void release_capture_resources(void)
{
    if (s_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    }

    for (size_t i = 0; i < PREVIEW_BUFFER_COUNT; ++i) {
        if (s_capture_buffers[i].address != NULL) {
            munmap(s_capture_buffers[i].address, s_capture_buffers[i].length);
            s_capture_buffers[i].address = NULL;
            s_capture_buffers[i].length = 0;
        }
    }

    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

/* 将 V4L2 DQBUF 得到的 RGB565_BE（大端序）逐像素字节交换后
 * 写入 LVGL canvas 的 RGB565_LE（小端序）缓冲区，然后刷新画布。
 * OV3660 传感器输出大端序，LVGL 期望小端序，这里做转换。 */
static bool update_preview_frame(const void *frame, size_t frame_bytes)
{
    const size_t expected_bytes = (size_t)s_mode.width * s_mode.height * sizeof(uint16_t);
    if (frame_bytes < expected_bytes || s_canvas_buffer == NULL || s_canvas == NULL) {
        return false;
    }
    if (!board_service_display_lock(50)) {
        return false;
    }

    const uint8_t *source = frame;
    for (size_t pixel = 0; pixel < (size_t)s_mode.width * s_mode.height; ++pixel) {
        s_canvas_buffer[pixel] = ((uint16_t)source[pixel * 2U] << 8U) |
                                 (uint16_t)source[pixel * 2U + 1U];
    }

    lv_obj_invalidate(s_canvas);
    if (s_face_overlay != NULL) {
        lv_obj_invalidate(s_face_overlay);
    }

    ++s_frame_count;
    ++s_fps_window_frames;
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - s_fps_window_start_us;
    if (elapsed >= 1000000) {
        const uint32_t fps_x10 = (uint32_t)((uint64_t)s_fps_window_frames * 10000000ULL / (uint64_t)elapsed);
        if (s_status_label != NULL) {
            lv_label_set_text_fmt(s_status_label,
                                  "OV3660 %ux%u  %" PRIu32 ".%" PRIu32
                                  " fps  RGB:%" PRIu32 "  JPEG:%" PRIu32 "  THR:%.2f",
                                  s_mode.width, s_mode.height,
                                  fps_x10 / 10U, fps_x10 % 10U, s_frame_count,
                                  jpeg_service_encoded_count(),
                                  (double)face_recognition_service_threshold_get());
        }
        s_fps_window_start_us = now;
        s_fps_window_frames = 0;
    }

    board_service_display_unlock();
    return true;
}

/* 每 5 帧对检测到的人脸执行一次识别，更新识别标签和状态机。
 * 通过余弦相似度匹配确认人脸身份，状态机实现连续确认后才稳定显示。 */
static void process_face_recognition(const face_detection_results_t *faces)
{
    if (!face_recognition_service_is_ready()) {
        return;
    }

    for (uint8_t i = 0; i < FACE_DETECTION_MAX_RESULTS; ++i) {
        s_recognition_labels[i].valid = false;
    }

    const int64_t now_us = esp_timer_get_time();
    for (uint8_t i = 0; i < faces->count; ++i) {
        const face_detection_box_t *box = &faces->boxes[i];

        if (box->width == 0 || box->height == 0) {
            continue;
        }

        const int16_t *kp = box->keypoint;
        int16_t est[10];
        uint16_t recog_id = 0;
        float confidence = 0.0f;
        if (kp[0] == 0 && kp[1] == 0 && kp[4] == 0 && kp[5] == 0) {
            int cx = box->x + box->width / 2;
            int cy = box->y + box->height / 2;
            est[0] = cx - box->width * 15 / 100;
            est[1] = cy - box->height * 12 / 100;
            est[2] = cx - box->width * 15 / 100;
            est[3] = cy + box->height * 30 / 100;
            est[4] = cx;
            est[5] = cy + box->height * 8 / 100;
            est[6] = cx + box->width * 15 / 100;
            est[7] = cy - box->height * 12 / 100;
            est[8] = cx + box->width * 15 / 100;
            est[9] = cy + box->height * 30 / 100;
            kp = est;
            ESP_LOGI(TAG, "detect #%u: box(%d,%d %dx%d) score=%u kp=est",
                     i, box->x, box->y, box->width, box->height, box->score_x10000);
        } else {
            ESP_LOGI(TAG, "detect #%u: box(%d,%d %dx%d) score=%u kp=model",
                     i, box->x, box->y, box->width, box->height, box->score_x10000);
        }
        face_recognition_service_recognize(s_canvas_buffer, s_mode.width, s_mode.height,
                                            kp, &recog_id, &confidence);

        face_recognition_state_t *state = &s_recognition_states[i];

        /* 人脸识别确认状态机：
         * - pending_id: 当前候选 ID
         * - pending_matches: 连续匹配次数（需 >= FACE_CONFIRM_MATCHES 才确认）
         * - confirmed_id: 已确认的 ID（非零时显示）
         * - last_confirmed_match_us: 最近一次确认时间
         *
         * 状态转换：
         *   识别到 id≠0 → pending 计数累加 → 达标后 confirmed
         *   识别到 id=0 → 清空 pending（不立即清 confirmed——保留 FACE_HOLD_TIME_US）
         *   超时 FACE_HOLD_TIME_US 且无匹配 → 清空 confirmed
         */
        if (recog_id != 0) {
            if (state->pending_id == recog_id) {
                if (state->pending_matches < UINT8_MAX) {
                    ++state->pending_matches;
                }
            } else {
                state->pending_id = recog_id;
                state->pending_matches = 1;
            }

            if (state->pending_matches >= FACE_CONFIRM_MATCHES) {
                state->confirmed_id = recog_id;
                state->confirmed_confidence = confidence;
                state->last_confirmed_match_us = now_us;
            } else if (state->confirmed_id == recog_id) {
                /* 已确认同 ID，仅更新置信度 */
                state->confirmed_confidence = confidence;
                state->last_confirmed_match_us = now_us;
            }
        } else {
            state->pending_id = 0;
            state->pending_matches = 0;
        }

        /* 判定当前帧该显示什么：
         * - 已确认且未超时 → 使用 confirmed 的 ID
         * - 未确认但 pending 未达标 → 显示 0（unknown）
         */
        if (state->confirmed_id != 0 &&
            now_us - state->last_confirmed_match_us <= FACE_HOLD_TIME_US) {
            recog_id = state->confirmed_id;
            confidence = state->confirmed_confidence;
        } else if (state->pending_matches < FACE_CONFIRM_MATCHES) {
            state->confirmed_id = 0;
            state->confirmed_confidence = 0.0f;
            recog_id = 0;
            confidence = 0.0f;
        }

        s_recognition_labels[i].id = recog_id;
        s_recognition_labels[i].confidence = confidence;
        s_recognition_labels[i].x = box->x;
        s_recognition_labels[i].y = box->y;
        s_recognition_labels[i].width = box->width;
        s_recognition_labels[i].height = box->height;
        s_recognition_labels[i].valid = true;

        ESP_LOGI(TAG, "recog #%u: id=%u conf=%.3f thr=%.3f %s",
                 i, recog_id, (double)confidence,
                 (double)face_recognition_service_threshold_get(),
                 recog_id != 0 ? "HIT" : "MISS");
    }
}

/* 摄像头采集后台任务：
 * DQBUF 获取 V4L2 帧 → 人脸检测提交 → 每 5 帧人脸识别 →
 * 更新 LVGL 预览画布 → 硬件 JPEG 编码 → 分发到 TCP/MJPEG/录像 */
static void capture_task(void *arg)
{
    (void)arg;
    uint32_t consecutive_timeouts = 0;

    while (s_running) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_fd, VIDIOC_DQBUF, &buffer) != 0) {
            if (!s_running) {
                break;
            }
            if (++consecutive_timeouts == 50U) {
                ESP_LOGE(TAG, "camera produced no frame for 10 seconds");
                break;
            }
            continue;
        }

        consecutive_timeouts = 0;
        if (buffer.index < PREVIEW_BUFFER_COUNT &&
            (buffer.flags & V4L2_BUF_FLAG_DONE) != 0 && buffer.bytesused > 0) {
            if (update_preview_frame(s_capture_buffers[buffer.index].address, buffer.bytesused)) {
                if ((s_frame_count % FACE_DETECTION_INTERVAL) == 0U) {
                    (void)face_detection_service_submit_rgb565(s_canvas_buffer,
                                                                s_mode.width, s_mode.height,
                                                                s_frame_count);
                    face_detection_results_t latest_faces = {0};
                    if (face_detection_service_get_latest(&latest_faces) &&
                        latest_faces.source_width == s_mode.width &&
                        latest_faces.source_height == s_mode.height) {
                        process_face_recognition(&latest_faces);
                        if (s_face_overlay != NULL) {
                            lv_obj_invalidate(s_face_overlay);
                        }
                    }
                }
                esp_err_t encode_ret = jpeg_service_encode_rgb565(s_canvas_buffer,
                                                                   (size_t)s_mode.width * s_mode.height *
                                                                       sizeof(uint16_t),
                                                                   esp_timer_get_time());
                if (encode_ret != ESP_OK && encode_ret != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGW(TAG, "JPEG frame skipped: %s", esp_err_to_name(encode_ret));
                }

                /* 检查是否有本地触屏触发的注册通知（非阻塞），
                 * 使用当前已检测到的人脸框执行特征提取和注册。 */
                uint32_t enroll_notify = 0;
                if (xTaskNotifyWait(0, 0, &enroll_notify, 0) == pdTRUE) {
                    uint8_t face_index = (uint8_t)enroll_notify;
                    face_detection_results_t latest = {0};
                    if (face_detection_service_get_latest(&latest) && face_index < latest.count) {
                        const face_detection_box_t *box = &latest.boxes[face_index];
                        if (box->width > 0 && box->height > 0) {
                            uint16_t new_id = 0;
                            esp_err_t err = face_recognition_service_enroll(
                                s_canvas_buffer, s_mode.width, s_mode.height,
                                box->x, box->y, box->width, box->height,
                                box->keypoint,
                                &new_id);
                            if (err == ESP_OK) {
                                ESP_LOGI(TAG, "face enrolled as ID %u", new_id);
                            } else {
                                ESP_LOGW(TAG, "face enrollment failed: %s", esp_err_to_name(err));
                            }
                        }
                    }
                }
            }
        }

        if (ioctl(s_fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed: errno=%d", errno);
            break;
        }
    }

    s_running = false;
    release_capture_resources();
    ESP_LOGI(TAG, "capture task stopped after %lu frames", (unsigned long)s_frame_count);
    s_capture_task = NULL;
    vTaskDelete(NULL);
}

/* 创建预览 UI：标题栏 + RGB565 画布 + 透明人脸框叠加层 + 状态标签 */
static esp_err_t create_preview_ui(void)
{
    ESP_RETURN_ON_FALSE(board_service_display() != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    ESP_RETURN_ON_FALSE(board_service_display_lock(1000), ESP_ERR_TIMEOUT, TAG,
                        "display lock timed out");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ESP32-S31 CAMERA PREVIEW555");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(title, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(title, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(title, 6, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buffer, s_mode.width, s_mode.height,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_border_color(s_canvas, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_canvas, 2, LV_PART_MAIN);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    /* 创建透明人脸框叠加层：位于画布上方，无视觉样式，
     * 仅通过 DRAW_MAIN 事件钩子绘制人脸框和标签文本。 */
    s_face_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_face_overlay);
    lv_obj_set_size(s_face_overlay, s_mode.width, s_mode.height);
    lv_obj_align(s_face_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_face_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_face_overlay, face_overlay_draw, LV_EVENT_DRAW_MAIN, NULL);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "Starting OV3660...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_label, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_status_label, 6, LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_move_foreground(title);

    board_service_display_unlock();
    return ESP_OK;
}

/* 配置 V4L2 采集设备：打开、超时、传感器格式、V4L2 格式、
 * 翻转校正（摄像头倒置安装）、双缓冲 MMAP 映射、STREAMON */
static esp_err_t configure_capture_device(void)
{
    esp_err_t ret = ESP_OK;
    s_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG, "open %s failed: errno=%d",
                        ESP_VIDEO_DVP_DEVICE_NAME, errno);

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = DQBUF_TIMEOUT_MS * 1000U,
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) == 0, ESP_FAIL, fail, TAG,
                      "VIDIOC_S_DQBUF_TIMEOUT failed: errno=%d", errno);

    ESP_GOTO_ON_ERROR(board_service_camera_set_rgb565_mode(s_fd, s_mode.width, s_mode.height),
                      fail, TAG, "sensor format selection failed");

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = s_mode.width,
            .height = s_mode.height,
            .pixelformat = V4L2_PIX_FMT_RGB565,
        },
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, fail, TAG,
                      "VIDIOC_S_FMT failed: errno=%d", errno);

    /* The Korvo camera is physically mounted upside down. Apply the sensor-side
     * 180-degree correction before STREAMON so every future frame has the same
     * orientation, including JPEG and RGB565 capture paths. */
    struct v4l2_ext_control orientation = {
        .id = V4L2_CID_VFLIP,
        .value = 1,
    };
    struct v4l2_ext_controls orientation_controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count = 1,
        .controls = &orientation,
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &orientation_controls) == 0,
                      ESP_FAIL, fail, TAG,
                      "VFLIP control failed: errno=%d", errno);
    orientation.id = V4L2_CID_HFLIP;
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &orientation_controls) == 0,
                      ESP_FAIL, fail, TAG,
                      "HFLIP control failed: errno=%d", errno);
    ESP_LOGI(TAG, "sensor orientation corrected: HFLIP+VFLIP");

    struct v4l2_requestbuffers request = {
        .count = PREVIEW_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &request) == 0, ESP_ERR_NO_MEM, fail, TAG,
                      "VIDIOC_REQBUFS failed: errno=%d, largest PSRAM block=%u", errno,
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_GOTO_ON_FALSE(request.count >= PREVIEW_BUFFER_COUNT, ESP_ERR_NO_MEM, fail, TAG,
                      "driver provided only %lu capture buffers", (unsigned long)request.count);

    for (uint32_t i = 0; i < PREVIEW_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &buffer) == 0, ESP_FAIL, fail, TAG,
                          "VIDIOC_QUERYBUF[%lu] failed: errno=%d", (unsigned long)i, errno);

        void *mapped = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                            s_fd, buffer.m.offset);
        ESP_GOTO_ON_FALSE(mapped != MAP_FAILED, ESP_ERR_NO_MEM, fail, TAG,
                          "mmap[%lu] failed: errno=%d", (unsigned long)i, errno);
        s_capture_buffers[i].address = mapped;
        s_capture_buffers[i].length = buffer.length;

        ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buffer) == 0, ESP_FAIL, fail, TAG,
                          "VIDIOC_QBUF[%lu] failed: errno=%d", (unsigned long)i, errno);
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, fail, TAG,
                      "VIDIOC_STREAMON failed: errno=%d, largest PSRAM block=%u", errno,
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "capture configured: %ux%u RGB565, %u MMAP buffers",
             s_mode.width, s_mode.height, PREVIEW_BUFFER_COUNT);
    return ESP_OK;

fail:
    release_capture_resources();
    return ret;
}

#define FACE_NAME_NVS_NAMESPACE "media_cfg"
#define FACE_NAME_NVS_KEY       "face_names"

/* 持久化人脸姓名表到 NVS（media_cfg 命名空间，blob 格式）
 * 与 SPIFFS 人脸特征库分离存储，方便独立修改和持久化。
 * 使用 NVS blob 一次性读写整个姓名表（FACE_NAME_MAX × 32 字节 ≈ 3.2KB）。 */
static void save_face_names_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(FACE_NAME_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS for face names");
        return;
    }
    size_t blob_size = sizeof(s_face_names);
    nvs_set_blob(handle, FACE_NAME_NVS_KEY, s_face_names, blob_size);
    nvs_commit(handle);
    nvs_close(handle);
}

/* 启动时从 NVS 恢复人脸姓名表 */
static void load_face_names_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(FACE_NAME_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t blob_size = sizeof(s_face_names);
    esp_err_t err = nvs_get_blob(handle, FACE_NAME_NVS_KEY, s_face_names, &blob_size);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded face names from NVS");
    }
}

/* [媒体服务 初始化] 分配 LVGL canvas 缓冲区、创建预览 UI、加载人脸姓名 */
esp_err_t media_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(board_service_camera_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "camera BSP is not ready");

    reset_capture_buffers();
    s_mode_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mode_lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "media mode mutex allocation failed");
    s_canvas_buffer = heap_caps_aligned_alloc(64, PREVIEW_MAX_FRAME_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    ESP_RETURN_ON_FALSE(s_canvas_buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "preview canvas allocation failed");
    memset(s_canvas_buffer, 0, PREVIEW_MAX_FRAME_BYTES);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    load_face_names_from_nvs();

    ESP_RETURN_ON_ERROR(create_preview_ui(), TAG, "preview UI creation failed");
    ESP_RETURN_ON_ERROR(jpeg_service_init(s_mode.width, s_mode.height, JPEG_QUALITY), TAG,
                        "JPEG pipeline initialization failed");
    s_initialized = true;
    ESP_LOGI(TAG, "media pipeline initialized (%u-byte PSRAM preview canvas)",
             (unsigned)PREVIEW_MAX_FRAME_BYTES);
    return ESP_OK;
}

/* [媒体服务 启动] 配置 V4L2 采集、创建采集任务 */
esp_err_t media_service_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media service is not initialized");
    if (s_running) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_capture_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "previous capture task is still stopping");
    ESP_RETURN_ON_FALSE(s_mode.format == MEDIA_PIXEL_FORMAT_RGB565,
                        ESP_ERR_NOT_SUPPORTED, TAG, "only RGB565 preview is implemented");

    ESP_RETURN_ON_ERROR(configure_capture_device(), TAG, "camera capture configuration failed");

    s_frame_count = 0;
    s_fps_window_frames = 0;
    s_fps_window_start_us = esp_timer_get_time();
    s_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(capture_task, "camera_capture",
                                                 CAPTURE_TASK_STACK_BYTES, NULL,
                                                 CAPTURE_TASK_PRIORITY, &s_capture_task,
                                                 CAPTURE_TASK_CORE);
    if (created != pdPASS) {
        s_running = false;
        release_capture_resources();
        s_capture_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "local preview started");
    return ESP_OK;
}

/* [媒体服务 停止] 停止采集流，等待采集任务退出（带 3 秒超时） */
esp_err_t media_service_stop(void)
{
    if (!s_running && s_capture_task == NULL) {
        return ESP_OK;
    }

    s_running = false;
    if (s_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    }

    uint32_t waited_ms = 0;
    while (s_capture_task != NULL && waited_ms < STOP_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    ESP_RETURN_ON_FALSE(s_capture_task == NULL, ESP_ERR_TIMEOUT, TAG,
                        "capture task did not stop within %u ms", STOP_TIMEOUT_MS);
    return ESP_OK;
}

/* 摄像头模式切换：仅支持 640x480 和 240x240 RGB565，
 * 切换时暂停采集流 → 重配置传感器 + JPEG 编码器 → 重启采集。 */
esp_err_t media_service_set_mode(const media_mode_t *mode)
{
    ESP_RETURN_ON_FALSE(mode != NULL && mode->width > 0 && mode->height > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid media mode");
    ESP_RETURN_ON_FALSE(s_initialized && s_mode_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "media service is not initialized");
    ESP_RETURN_ON_FALSE(mode->format == MEDIA_PIXEL_FORMAT_RGB565 &&
                        ((mode->width == 640 && mode->height == 480) ||
                         (mode->width == 240 && mode->height == 240)),
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported OV3660 RGB565 mode %ux%u",
                        mode->width, mode->height);

    xSemaphoreTake(s_mode_lock, portMAX_DELAY);
    if (mode->width == s_mode.width && mode->height == s_mode.height &&
        mode->format == s_mode.format && mode->jpeg_quality == s_mode.jpeg_quality) {
        xSemaphoreGive(s_mode_lock);
        return ESP_OK;
    }

    const media_mode_t previous = s_mode;
    const bool restart = media_service_is_running();
    esp_err_t ret = restart ? media_service_stop() : ESP_OK;
    if (ret == ESP_OK) {
        ret = jpeg_service_reconfigure(mode->width, mode->height, mode->jpeg_quality,
                                       STOP_TIMEOUT_MS);
    }
    if (ret == ESP_OK && board_service_display_lock(1000)) {
        memset(s_canvas_buffer, 0, PREVIEW_MAX_FRAME_BYTES);
        lv_canvas_set_buffer(s_canvas, s_canvas_buffer, mode->width, mode->height,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s_face_overlay, mode->width, mode->height);
        lv_obj_center(s_face_overlay);
        lv_obj_center(s_canvas);
        lv_obj_invalidate(s_canvas);
        board_service_display_unlock();
        s_mode = *mode;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_TIMEOUT;
    }
    if (ret == ESP_OK && restart) {
        ret = media_service_start();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mode switch to %ux%u failed: %s; restoring %ux%u",
                 mode->width, mode->height, esp_err_to_name(ret),
                 previous.width, previous.height);
        s_mode = previous;
        (void)jpeg_service_reconfigure(previous.width, previous.height,
                                       previous.jpeg_quality, STOP_TIMEOUT_MS);
        if (board_service_display_lock(1000)) {
            lv_canvas_set_buffer(s_canvas, s_canvas_buffer, previous.width, previous.height,
                                 LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(s_face_overlay, previous.width, previous.height);
            lv_obj_center(s_face_overlay);
            lv_obj_center(s_canvas);
            lv_obj_invalidate(s_canvas);
            board_service_display_unlock();
        }
        if (restart && !media_service_is_running()) {
            (void)media_service_start();
        }
    } else {
        ESP_LOGI(TAG, "media mode switched to %ux%u quality=%u", mode->width, mode->height,
                 mode->jpeg_quality);
    }
    xSemaphoreGive(s_mode_lock);
    return ret;
}

/* [媒体服务 模式查询] 获取当前分辨率/格式/JPEG 质量设置 */
esp_err_t media_service_get_mode(media_mode_t *mode)
{
    ESP_RETURN_ON_FALSE(mode != NULL, ESP_ERR_INVALID_ARG, TAG, "mode is null");
    ESP_RETURN_ON_FALSE(s_initialized && s_mode_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "media service is not initialized");
    xSemaphoreTake(s_mode_lock, portMAX_DELAY);
    *mode = s_mode;
    xSemaphoreGive(s_mode_lock);
    return ESP_OK;
}

bool media_service_is_running(void)
{
    return s_running || s_capture_task != NULL;
}

uint32_t media_service_frame_count(void)
{
    return s_frame_count;
}

/* [媒体服务 识别标签] 获取最新人脸识别标签列表（供 TCP 协议层 / 上位机使用） */
bool media_service_get_recognition_labels(media_face_recognition_t *out, uint8_t *out_count)
{
    if (out == NULL || out_count == NULL) {
        return false;
    }
    uint8_t count = 0;
    for (uint8_t i = 0; i < FACE_DETECTION_MAX_RESULTS && count < MEDIA_FACE_RECOGNITION_MAX; ++i) {
        if (s_recognition_labels[i].valid) {
            out[count].id = s_recognition_labels[i].id;
            out[count].confidence = s_recognition_labels[i].confidence;
            ++count;
        }
    }
    *out_count = count;
    return true;
}

/* 本地触屏人脸注册：向采集任务发送通知，采集任务在下一帧时提取特征。
 * face_index 是当前帧人脸检测结果中的下标。 */
esp_err_t media_service_enroll_face(uint8_t face_index)
{
    if (s_capture_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_capture_task, (uint32_t)face_index, eSetValueWithOverwrite);
    return ESP_OK;
}

/* 上位机上传人脸注册：接收裁剪后的 RGB565 图像，同步执行检测→特征提取→写入数据库。
 * 成功时 out_id 返回新分配的人脸 ID。 */
esp_err_t media_service_enroll_face_upload(const uint16_t *rgb565,
                                            uint16_t crop_w, uint16_t crop_h,
                                            uint16_t *out_id)
{
    if (rgb565 == NULL || out_id == NULL || crop_w == 0 || crop_h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!face_recognition_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    face_detection_results_t det_result;
    esp_err_t err = face_detection_service_detect_sync(rgb565, crop_w, crop_h, &det_result);
    if (err != ESP_OK || det_result.count == 0) {
        ESP_LOGW(TAG, "upload enrollment: detection failed (%s, %u faces)",
                 esp_err_to_name(err), det_result.count);
        return (err == ESP_OK) ? ESP_ERR_NOT_FOUND : err;
    }

    const face_detection_box_t *box = &det_result.boxes[0];
    return face_recognition_service_enroll(rgb565, crop_w, crop_h,
                                            box->x, box->y, box->width, box->height,
                                            box->keypoint,
                                            out_id);
}

/* [媒体服务 设置姓名] 设置指定人脸 ID 的显示姓名，并立即持久化到 NVS */
void media_service_set_face_name(uint16_t id, const char *name)
{
    if (id >= FACE_NAME_MAX || name == NULL) {
        return;
    }
    strncpy(s_face_names[id], name, FACE_NAME_LEN - 1);
    s_face_names[id][FACE_NAME_LEN - 1] = '\0';
    ESP_LOGI(TAG, "face ID %u named \"%s\"", id, s_face_names[id]);
    save_face_names_to_nvs();
}

/* [媒体服务 清空姓名] 清空所有人脸姓名映射并持久化 */
void media_service_clear_face_names(void)
{
    memset(s_recognition_states, 0, sizeof(s_recognition_states));
    memset(s_face_names, 0, sizeof(s_face_names));
    save_face_names_to_nvs();
}

/* [媒体服务 人脸列表] 获取当前已命名的人脸 ID 和姓名列表 */
esp_err_t media_service_get_face_list(media_face_list_entry_t *out, uint8_t *out_count)
{
    if (out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t count = 0;
    for (uint16_t i = 1; i < FACE_NAME_MAX && count < MEDIA_FACE_LIST_MAX; i++) {
        if (s_face_names[i][0] != '\0') {
            out[count].id = i;
            strncpy(out[count].name, s_face_names[i], sizeof(out[count].name) - 1);
            out[count].name[sizeof(out[count].name) - 1] = '\0';
            count++;
        }
    }
    *out_count = count;
    return ESP_OK;
}