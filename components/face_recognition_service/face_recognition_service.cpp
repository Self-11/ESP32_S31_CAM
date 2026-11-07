/* 人脸识别服务实现
 * 基于 ESP-DL MobileFaceNet (MFN) 模型进行人脸特征提取。
 * 使用余弦相似度比对，支持人脸注册、删除、清空和基于关键点的识别。
 * 人脸特征库存储在 SPIFFS 中，通过 ESP-DL 的 RecognitionDatabase 管理。
 */

#include "face_recognition_service.h"

#include <list>
#include <vector>
#include <sys/stat.h>

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "dl_recognition_database.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "human_face_recognition.hpp"

namespace {

constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 3;
constexpr BaseType_t kTaskCore = 1;          /* 绑定到 CPU1 */
constexpr float kDefaultThreshold = 0.30f;   /* 默认识别阈值 */
constexpr float kMinimumTop2Margin = 0.08f;  /* Top-2 最小间隔 */

const char *TAG = "face_recog";

SemaphoreHandle_t s_input_lock = nullptr;    /* 保护识别器线程安全 */
TaskHandle_t s_task = nullptr;
volatile bool s_enabled = true;
volatile bool s_ready = false;

HumanFaceRecognizer *s_recognizer = nullptr; /* ESP-DL 识别器实例 */

volatile float s_threshold = kDefaultThreshold;
volatile uint32_t s_inference_ms = 0;

/* 基于人脸框估算 5 点关键点（无关键点时作为回退方案）
 * 关键点顺序：左眼(x,y)、左嘴角(x,y)、鼻尖(x,y)、右眼(x,y)、右嘴角(x,y) */

std::vector<int> estimate_keypoints(uint16_t face_x,
                                    uint16_t face_y,
                                    uint16_t face_w,
                                    uint16_t face_h)
{
    const float cx = static_cast<float>(face_x) + static_cast<float>(face_w) * 0.5f;
    const float cy = static_cast<float>(face_y) + static_cast<float>(face_h) * 0.5f;

    const float left_eye_x  = cx - static_cast<float>(face_w) * 0.15f;
    const float left_eye_y  = cy - static_cast<float>(face_h) * 0.12f;
    const float left_mouth_x = cx - static_cast<float>(face_w) * 0.15f;
    const float left_mouth_y = cy + static_cast<float>(face_h) * 0.30f;
    const float nose_x       = cx;
    const float nose_y       = cy + static_cast<float>(face_h) * 0.08f;
    const float right_eye_x  = cx + static_cast<float>(face_w) * 0.15f;
    const float right_eye_y  = cy - static_cast<float>(face_h) * 0.12f;
    const float right_mouth_x = cx + static_cast<float>(face_w) * 0.15f;
    const float right_mouth_y = cy + static_cast<float>(face_h) * 0.30f;

    return {
        static_cast<int>(left_eye_x),  static_cast<int>(left_eye_y),
        static_cast<int>(left_mouth_x), static_cast<int>(left_mouth_y),
        static_cast<int>(nose_x),       static_cast<int>(nose_y),
        static_cast<int>(right_eye_x),  static_cast<int>(right_eye_y),
        static_cast<int>(right_mouth_x), static_cast<int>(right_mouth_y),
    };
}

/* 将人脸检测框坐标和可选的关键点组装为 ESP-DL detect::result_t。
 * 如果关键点全为零（检测器未返回关键点），则基于人脸框位置估算 5 点关键点，
 * 估算公式按人脸比例：左右眼在框上方 12%、左右嘴角在下方 30%、鼻尖在中心偏下 8%。 */
dl::detect::result_t make_detect_result(uint16_t face_x,
                                        uint16_t face_y,
                                        uint16_t face_w,
                                        uint16_t face_h,
                                        const int16_t *keypoints = nullptr)
{
    dl::detect::result_t det;
    det.category = 0;
    det.score = 1.0f;
    det.box = {
        static_cast<int>(face_x),
        static_cast<int>(face_y),
        static_cast<int>(face_x + face_w),
        static_cast<int>(face_y + face_h),
    };
    bool has_keypoints = false;
    if (keypoints != nullptr) {
        for (int i = 0; i < 10; ++i) {
            has_keypoints = has_keypoints || keypoints[i] != 0;
        }
    }
    if (has_keypoints) {
        det.keypoint = {
            static_cast<int>(keypoints[0]), static_cast<int>(keypoints[1]),
            static_cast<int>(keypoints[2]), static_cast<int>(keypoints[3]),
            static_cast<int>(keypoints[4]), static_cast<int>(keypoints[5]),
            static_cast<int>(keypoints[6]), static_cast<int>(keypoints[7]),
            static_cast<int>(keypoints[8]), static_cast<int>(keypoints[9]),
        };
    } else {
        det.keypoint = estimate_keypoints(face_x, face_y, face_w, face_h);
    }
    return det;
}

/* 人脸识别服务后台任务：
 * - 绑定到 ESP-DL HumanFaceRecognizer 模型（MFN 512维特征）
 * - 从 SPIFFS 加载已注册人脸特征库 face_db.bin
 * - 设置为 Top-K=2 获取最佳匹配和次佳匹配用于 margin 判断
 * - 初始化完成后挂起等待通知（实际识别由调用方通过 recognize() 同步执行）
 */
void recognition_task(void *)
{
#if CONFIG_HUMAN_FACE_FEAT_MODEL_IN_SDCARD
    const char *model_path = "/sdcard/" CONFIG_HUMAN_FACE_FEAT_MODEL_SDCARD_DIR
                             "/human_face_feat_mfn_s8_v1.espdl";
    struct stat st;
    if (stat(model_path, &st) != 0) {
        ESP_LOGE(TAG, "Model file not found: %s", model_path);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "model file found: %s (%ld bytes)", model_path, st.st_size);
#endif

    const char *db_path = CONFIG_FACE_RECOGNITION_DB_PATH;
    s_recognizer = new (std::nothrow) HumanFaceRecognizer(db_path);
    if (s_recognizer == nullptr) {
        ESP_LOGE(TAG, "HumanFaceRecognizer allocation failed");
        vTaskDelete(nullptr);
        return;
    }
    s_recognizer->set_threshold(s_threshold);
    s_recognizer->set_top_k(2);

    int num_feats = s_recognizer->get_num_feats();
    s_recognizer->get_feat_model()->get_feat_len();

    s_ready = true;
    ESP_LOGI(TAG, "face recognition ready (threshold=%.2f, enrolled=%d)",
             static_cast<double>(s_threshold), num_feats);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_enabled) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

} // namespace

/* 初始化人脸识别服务：创建互斥锁和后台识别任务。
 * 模型加载和特征库恢复将在后台任务中异步完成。 */
extern "C" esp_err_t face_recognition_service_init(const char *db_path)
{
    if (s_task != nullptr) {
        return ESP_OK;
    }
    s_input_lock = xSemaphoreCreateMutex();
    if (s_input_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(recognition_task, "face_recog", kTaskStackBytes,
                                nullptr, kTaskPriority, &s_task, kTaskCore) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 设置识别服务启用/禁用标志 */
extern "C" esp_err_t face_recognition_service_set_enabled(bool enabled)
{
    s_enabled = enabled;
    return ESP_OK;
}

extern "C" bool face_recognition_service_is_enabled(void) { return s_enabled; }
extern "C" bool face_recognition_service_is_ready(void) { return s_ready; }

/* 对单个人脸区域执行识别：
 * 1. 将输入像素组装为 ESP-DL img_t
 * 2. 通过关键点构造检测结果引用
 * 3. 分两次查询：第一次阈值为 0 获取 Top-2 原始相似度
 * 4. Top-2 margin ≥ 0.08 且最佳匹配 ≥ 阈值才确认
 * 成功时返回 ESP_OK 并将 matched_id 和 confidence 写入 out 参数。
 * out_id=0 表示未匹配任何注册用户。 */
extern "C" esp_err_t face_recognition_service_recognize(const uint16_t *pixels,
                                                          uint16_t width,
                                                          uint16_t height,
                                                          const int16_t *keypoints,
                                                          uint16_t *out_id,
                                                          float *out_confidence)
{
    *out_id = 0;
    *out_confidence = 0.0f;

    if (!s_ready || s_recognizer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    dl::image::img_t image = {
        .data = const_cast<uint16_t *>(pixels),
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    dl::detect::result_t det = make_detect_result(0, 0, width, height, keypoints);
    std::list<dl::detect::result_t> det_res = {det};

    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    const int db_count = s_recognizer->get_num_feats();
    ESP_LOGI(TAG, "recog: img=%ux%u kp0=[%d,%d] kp4=[%d,%d] db=%d",
             width, height,
             keypoints != nullptr ? keypoints[0] : -999,
             keypoints != nullptr ? keypoints[1] : -999,
             keypoints != nullptr ? keypoints[4] : -999,
             keypoints != nullptr ? keypoints[5] : -999,
             db_count);

    const int64_t started = esp_timer_get_time();

    /* 临时降低阈值到 0 以获取 Top-2 原始相似度列表，
     * 用于计算 margin 判断是否为模糊匹配（第二名太近），也是一种确认机制 */
    const float saved_thr = s_recognizer->get_threshold();
    s_recognizer->set_threshold(0.0f);
    auto all_results = s_recognizer->recognize(image, det_res);
    s_recognizer->set_threshold(saved_thr);
    xSemaphoreGive(s_input_lock);

    s_inference_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);

    if (db_count > 0 && all_results.empty()) {
        ESP_LOGW(TAG, "recog: ALL %d features returned zero similarity!", db_count);
    } else {
        for (size_t i = 0; i < all_results.size() && i < 5; i++) {
            ESP_LOGI(TAG, "recog raw[%u]: id=%u sim=%.4f",
                     (unsigned)i, all_results[i].id,
                     (double)all_results[i].similarity);
        }
    }

    /* 分析 Top-2 结果：
     * - 最佳匹配相似度必须高于阈值
     * - 第二名与第一名的差距必须 >= kMinimumTop2Margin (0.08)，
     *   否则认为是模糊匹配，拒绝返回结果 */
    if (!all_results.empty()) {
        const auto &best = all_results.front();
        const float runner_up = all_results.size() > 1 ? all_results[1].similarity : 0.0f;
        const float margin = best.similarity - runner_up;
        const bool separated = all_results.size() < 2 || margin >= kMinimumTop2Margin;
        if (best.similarity > saved_thr && separated) {
            *out_id = best.id;
            *out_confidence = best.similarity;
            ESP_LOGI(TAG, "recog MATCH: id=%u sim=%.4f margin=%.4f thr=%.2f",
                     best.id, (double)best.similarity, (double)margin, (double)saved_thr);
        } else if (best.similarity > saved_thr) {
            ESP_LOGI(TAG, "recog AMBIGUOUS: best=%u sim=%.4f second=%.4f margin=%.4f",
                     best.id, (double)best.similarity, (double)runner_up, (double)margin);
        }
    }
    ESP_LOGI(TAG, "recog result: id=%u conf=%.4f inf=%lums db=%d",
             *out_id, (double)*out_confidence, (unsigned long)s_inference_ms, db_count);
    return ESP_OK;
}

/* 人脸注册：将人脸框区域的 RGB565 像素提取特征向量后写入 SPIFFS face_db.bin。
 * ID 分配策略：新注册的 ID = 当前数据库特征总数（起始 ID=1，自增）。
 * 这意味着删除后重新注册会得到新 ID，不会复用旧 ID。 */
extern "C" esp_err_t face_recognition_service_enroll(const uint16_t *pixels,
                                                       uint16_t width,
                                                       uint16_t height,
                                                       uint16_t face_x,
                                                       uint16_t face_y,
                                                       uint16_t face_w,
                                                       uint16_t face_h,
                                                       const int16_t *keypoints,
                                                       uint16_t *out_id)
{
    if (out_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_id = 0;

    if (!s_ready || s_recognizer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    dl::image::img_t image = {
        .data = const_cast<uint16_t *>(pixels),
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    dl::detect::result_t det = make_detect_result(face_x, face_y, face_w, face_h, keypoints);

    ESP_LOGI(TAG, "enroll: img=%ux%u box=(%u,%u %ux%u) "
             "kp0=[%d,%d] kp4=[%d,%d]",
             width, height,
             face_x, face_y, face_w, face_h,
             det.keypoint[0], det.keypoint[1],
             det.keypoint[4], det.keypoint[5]);

    std::list<dl::detect::result_t> det_res = {det};

    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    esp_err_t ret = s_recognizer->enroll(image, det_res);
    const int total = s_recognizer->get_num_feats();
    if (ret == ESP_OK && out_id != nullptr) {
        *out_id = static_cast<uint16_t>(total);
    }
    xSemaphoreGive(s_input_lock);
    ESP_LOGI(TAG, "enroll result: ret=%d id=%u total=%d",
             (int)ret, *out_id, total);
    return ret;
}

/* 删除指定 ID 的用户特征（线程安全） */
extern "C" esp_err_t face_recognition_service_delete(uint16_t id)
{
    if (!s_ready || s_recognizer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    esp_err_t ret = s_recognizer->delete_feat(id);
    xSemaphoreGive(s_input_lock);
    return ret;
}

/* 清空所有已注册的用户特征 */
extern "C" esp_err_t face_recognition_service_clear(void)
{
    if (!s_ready || s_recognizer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    esp_err_t ret = s_recognizer->clear_all_feats();
    xSemaphoreGive(s_input_lock);
    return ret;
}

/* 查询已注册用户数量（用于生成新用户 ID） */
extern "C" uint16_t face_recognition_service_user_count(void)
{
    if (s_recognizer == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    const uint16_t count = static_cast<uint16_t>(s_recognizer->get_num_feats());
    xSemaphoreGive(s_input_lock);
    return count;
}

extern "C" float face_recognition_service_threshold_get(void) { return s_threshold; }

/* 动态设置识别相似度阈值（0.0-1.0），同时同步到 ESP-DL 识别器 */
extern "C" void face_recognition_service_threshold_set(float threshold)
{
    if (threshold < 0.0f) {
        threshold = 0.0f;
    } else if (threshold > 1.0f) {
        threshold = 1.0f;
    }
    s_threshold = threshold;
    if (s_recognizer != nullptr) {
        xSemaphoreTake(s_input_lock, portMAX_DELAY);
        s_recognizer->set_threshold(threshold);
        xSemaphoreGive(s_input_lock);
    }
}

extern "C" uint32_t face_recognition_service_inference_ms(void) { return s_inference_ms; }