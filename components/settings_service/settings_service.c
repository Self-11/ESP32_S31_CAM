/* 系统设置服务实现
 * 通过 LVGL 构建一个 Wi-Fi 设置页面，支持：
 * - SSID / 密码输入（软键盘）
 * - Wi-Fi 状态实时刷新
 * - 保存并连接路由器
 * - 设备 ID 显示
 */

#include "settings_service.h"

#include <stdbool.h>
#include <string.h>

#include "board_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"
#include "network_service.h"

static const char *TAG = "settings_service";
static lv_obj_t *s_page;                /* Wi-Fi 设置页面容器 */
static lv_obj_t *s_ssid;                /* SSID 输入框 */
static lv_obj_t *s_password;            /* 密码输入框 */
static lv_obj_t *s_keyboard;            /* LVGL 软键盘 */
static lv_obj_t *s_state_label;         /* 状态标签 */
static lv_obj_t *s_device_label;        /* 设备 ID 标签 */
static lv_timer_t *s_status_timer;      /* 状态刷新定时器（1 秒） */

/* 定时刷新网络状态和 IP 地址 */
static void refresh_status(lv_timer_t *timer)
{
    (void)timer;
    network_status_t status = {0};
    if (network_service_get_status(&status) != ESP_OK || s_state_label == NULL) {
        return;
    }
    const char *mode = status.sta_connected ? "Router connected" :
                       status.fallback_ap_active ? "Setup hotspot active" : "Connecting...";
    lv_label_set_text_fmt(s_state_label, "%s   IP: %s", mode, status.ip);
    lv_label_set_text_fmt(s_device_label, "Device ID: %s", status.device_id);
}

/* 文本区域获得焦点时弹出软键盘 */
static void textarea_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_FOCUSED) {
        return;
    }
    lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
    lv_obj_set_hidden(s_keyboard, false);
    lv_obj_move_foreground(s_keyboard);
}

/* 软键盘确认/取消时隐藏 */
static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_set_hidden(s_keyboard, true);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

/* 关闭设置页面 */
static void close_event(lv_event_t *event)
{
    (void)event;
    lv_obj_set_hidden(s_page, true);
    lv_obj_set_hidden(s_keyboard, true);
}

/* 打开设置页面，加载当前 Wi-Fi 凭证 */
static void open_event(lv_event_t *event)
{
    (void)event;
    network_wifi_config_t config = {0};
    if (network_service_get_wifi_config(&config) == ESP_OK) {
        lv_textarea_set_text(s_ssid, config.ssid);
        lv_textarea_set_text(s_password, config.password);
    }
    refresh_status(NULL);
    lv_obj_set_hidden(s_page, false);
    lv_obj_move_foreground(s_page);
}

/* 保存 Wi-Fi 设置并触发连接 */
static void save_event(lv_event_t *event)
{
    (void)event;
    network_wifi_config_t config = {0};
    strlcpy(config.ssid, lv_textarea_get_text(s_ssid), sizeof(config.ssid));
    strlcpy(config.password, lv_textarea_get_text(s_password), sizeof(config.password));

    esp_err_t ret = network_service_set_wifi_config(&config);
    if (ret == ESP_OK) {
        lv_label_set_text(s_state_label, "Saved. Connecting to router...");
        lv_obj_set_hidden(s_keyboard, true);
    } else {
        lv_label_set_text_fmt(s_state_label, "Invalid settings: %s", esp_err_to_name(ret));
    }
}

/* 创建带标签的文本输入字段 */
static lv_obj_t *create_field(lv_obj_t *parent, const char *label_text, int32_t y,
                              bool password)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_pos(label, 28, y + 10);

    lv_obj_t *field = lv_textarea_create(parent);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_password_mode(field, password);
    lv_obj_set_size(field, 520, 42);
    lv_obj_set_pos(field, 160, y);
    lv_obj_add_event_cb(field, textarea_event, LV_EVENT_FOCUSED, NULL);
    return field;
}

/* 初始化 Wi-Fi 设置 UI 界面
 * 创建设置按钮、设置页面、输入框、软键盘和状态刷新定时器 */
esp_err_t settings_service_init(void)
{
    ESP_RETURN_ON_FALSE(board_service_display() != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    ESP_RETURN_ON_FALSE(board_service_display_lock(1000), ESP_ERR_TIMEOUT, TAG,
                        "display lock timed out");

    lv_obj_t *screen = lv_screen_active();

    /* 右上角 Wi-Fi 设置入口按钮 */
    lv_obj_t *open_button = lv_button_create(screen);
    lv_obj_set_size(open_button, 72, 42);
    lv_obj_align(open_button, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(open_button, lv_color_hex(0x0284C7), 0);
    lv_obj_t *open_label = lv_label_create(open_button);
    lv_label_set_text(open_label, "Wi-Fi");
    lv_obj_center(open_label);
    lv_obj_add_event_cb(open_button, open_event, LV_EVENT_CLICKED, NULL);

    /* Wi-Fi 设置全屏页面 */
    s_page = lv_obj_create(screen);
    lv_obj_set_size(s_page, 800, 480);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);

    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Wi-Fi Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(title, 28, 14);

    s_device_label = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_device_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_pos(s_device_label, 28, 48);

    /* 输入字段：SSID 和密码 */
    s_ssid = create_field(s_page, "Wi-Fi name", 76, false);
    s_password = create_field(s_page, "Password", 124, true);
    lv_textarea_set_max_length(s_ssid, NETWORK_WIFI_SSID_MAX_LEN);
    lv_textarea_set_max_length(s_password, NETWORK_WIFI_PASSWORD_MAX_LEN);

    lv_obj_t *hint = lv_label_create(s_page);
    lv_label_set_text(hint, "IP is assigned automatically on first connection and reused later.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_pos(hint, 160, 180);

    /* 保存和返回按钮 */
    lv_obj_t *save = lv_button_create(s_page);
    lv_obj_set_size(save, 130, 44);
    lv_obj_set_pos(save, 160, 220);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save & Connect");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save, save_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close = lv_button_create(s_page);
    lv_obj_set_size(close, 100, 44);
    lv_obj_set_pos(close, 304, 220);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x475569), 0);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Back");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, close_event, LV_EVENT_CLICKED, NULL);

    s_state_label = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(s_state_label, 430, 234);

    /* 底部软键盘 */
    s_keyboard = lv_keyboard_create(s_page);
    lv_obj_set_size(s_keyboard, 800, 200);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_keyboard, true);

    /* 启动状态刷新定时器，初始页面隐藏 */
    s_status_timer = lv_timer_create(refresh_status, 1000, NULL);
    (void)s_status_timer;
    refresh_status(NULL);
    lv_obj_set_hidden(s_page, true);
    lv_obj_move_foreground(open_button);
    board_service_display_unlock();
    ESP_LOGI(TAG, "touch Wi-Fi settings page initialized");
    return ESP_OK;
}