/* 板级服务 - Korvo-1 V1.1 硬件抽象
 * 管理 RGB LCD (800x480)、GT1151 触摸屏、OV3660 DVP 摄像头、
 * 16MB Flash 和 16MB PSRAM 等板载外设的初始化和控制。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* 板级硬件参数 */
#define BOARD_LCD_WIDTH          800       /* LCD 显示宽度 */
#define BOARD_LCD_HEIGHT         480       /* LCD 显示高度 */
#define BOARD_FLASH_SIZE_BYTES   (16U * 1024U * 1024U)   /* Flash 容量 16MB */
#define BOARD_PSRAM_SIZE_BYTES   (16U * 1024U * 1024U)   /* PSRAM 容量 16MB */

/* 初始化 LCD 和摄像头硬件 */
esp_err_t board_service_init(void);
/* 启动板级服务 */
esp_err_t board_service_start(void);
/* 检查 OV3660 摄像头是否就绪 */
bool board_service_camera_ready(void);
/* 设置 OV3660 为指定分辨率的 RGB565 模式 */
esp_err_t board_service_camera_set_rgb565_mode(int video_fd, uint16_t width, uint16_t height);
/* 获取 LVGL 显示对象指针 */
lv_display_t *board_service_display(void);
/* 锁定 LVGL 显示（互斥访问，超时返回 false） */
bool board_service_display_lock(uint32_t timeout_ms);
/* 解锁 LVGL 显示 */
void board_service_display_unlock(void);