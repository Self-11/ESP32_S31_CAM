/* 板级服务实现
 * 驱动 RGB LCD 显示启动画面，初始化 OV3660 DVP 摄像头传感器，
 * 提供摄像头格式查询与模式切换功能。
 */

#include "board_service.h"

#include <stdbool.h>
#include <sys/ioctl.h>
#include "esp_cam_sensor.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_video_init.h"
#include "bsp/esp32_s31_korvo_1.h"

static const char *TAG = "board_service";
static bool s_initialized;
static bool s_started;
static bool s_camera_ready;
static lv_display_t *s_display;

/* esp_video 2.2 将此访问器放在设备层而非公共头文件中。
 * 在此声明以使用传感器驱动自带的格式表，避免复制 OV3660 寄存器表。 */
extern esp_cam_sensor_device_t *esp_video_get_dvp_video_device_sensor(void);

/* 初始化 RGB LCD 显示并绘制启动画面 */
static esp_err_t init_display(void)
{
    s_display = bsp_display_start();
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_FAIL, TAG, "RGB LCD/touch initialization failed");

    esp_err_t backlight_ret = bsp_display_backlight_on();
    if (backlight_ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "LCD backlight is not software-controlled; continuing with panel default");
    } else {
        ESP_RETURN_ON_ERROR(backlight_ret, TAG, "LCD backlight enable failed");
    }

    /* 在 LCD 上显示启动欢迎画面 */
    if (bsp_display_lock(1000)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, "ESP32-S31 Vision System\nHardware initialization complete");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(label);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "LCD initialized, but splash screen lock timed out");
    }

    ESP_LOGI(TAG, "RGB LCD and GT1151 touch initialized");
    return ESP_OK;
}

/* 启动 OV3660 DVP 摄像头传感器 */
static esp_err_t init_camera(void)
{
    ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG, "OV3660 sensor initialization failed");
    s_camera_ready = true;
    ESP_LOGI(TAG, "OV3660 DVP sensor registered at %s", BSP_CAMERA_DEVICE);
    return ESP_OK;
}

/* [板级服务 初始化] 创建 LVGL 互斥锁、初始化 LCD 和摄像头 */
esp_err_t board_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;              /* 已初始化，幂等操作 */
    }
    ESP_LOGI(TAG, "profile: Korvo-1 V1.1, LCD %dx%d RGB565, Flash/PSRAM 16/16 MB",
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    ESP_RETURN_ON_ERROR(init_display(), TAG, "display bring-up failed");
    ESP_RETURN_ON_ERROR(init_camera(), TAG, "camera bring-up failed");
    s_initialized = true;
    return ESP_OK;
}

/* [板级服务 摄像头] 摄像头初始化是否已完成 */
bool board_service_camera_ready(void)
{
    return s_camera_ready;
}

/* [板级服务 摄像头格式] 查询 OV3660 传感器支持的格式表，
 * 选择匹配的 RGB565 模式并应用至 V4L2 设备 */
esp_err_t board_service_camera_set_rgb565_mode(int video_fd, uint16_t width, uint16_t height)
{
    ESP_RETURN_ON_FALSE(s_camera_ready && video_fd >= 0, ESP_ERR_INVALID_STATE, TAG,
                        "camera is not ready");

    /* 获取 DVP 传感器句柄以查询其原生格式表 */
    esp_cam_sensor_device_t *sensor = esp_video_get_dvp_video_device_sensor();
    ESP_RETURN_ON_FALSE(sensor != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "DVP sensor handle is unavailable");

    esp_cam_sensor_format_array_t formats = {0};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(sensor, &formats), TAG,
                        "sensor format query failed");

    /* 遍历传感器支持的格式，查找匹配分辨率和 RGB565_BE 格式 */
    const esp_cam_sensor_format_t *selected = NULL;
    for (size_t i = 0; i < formats.count; ++i) {
        const esp_cam_sensor_format_t *candidate = &formats.format_array[i];
        if (candidate->width == width && candidate->height == height &&
            candidate->format == ESP_CAM_SENSOR_PIXFORMAT_RGB565_BE) {
            selected = candidate;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(selected != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                        "OV3660 RGB565 mode %ux%u is not enabled", width, height);
    ESP_RETURN_ON_FALSE(ioctl(video_fd, VIDIOC_S_SENSOR_FMT, selected) == 0,
                        ESP_FAIL, TAG, "sensor mode switch to %ux%u failed", width, height);
    ESP_LOGI(TAG, "OV3660 mode selected: %s", selected->name);
    return ESP_OK;
}

/* [板级服务 显示句柄] 返回当前 LVGL 显示设备指针 */
lv_display_t *board_service_display(void)
{
    return s_display;
}

/* [板级服务 锁定] 获取 LVGL 绘制互斥锁（带超时） */
bool board_service_display_lock(uint32_t timeout_ms)
{
    return s_display != NULL && bsp_display_lock(timeout_ms);
}

/* [板级服务 解锁] 释放 LVGL 绘制互斥锁 */
void board_service_display_unlock(void)
{
    if (s_display != NULL) {
        bsp_display_unlock();
    }
}

/* [板级服务 启动] 延时后轮询 LVGL 定时器 */
esp_err_t board_service_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_started) {
        ESP_LOGI(TAG, "board services started");
        s_started = true;
    }
    return ESP_OK;
}