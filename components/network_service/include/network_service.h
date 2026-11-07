/* 网络服务 - WiFi STA/AP 双模式管理
 * 支持 STA 模式连接路由器，失败时自动回退到 AP 模式提供配置热点。
 * 提供链接状态、IP、设备 ID 等运行时状态查询。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 网络链路类型枚举 */
typedef enum {
    NETWORK_LINK_NONE = 0,              /* 无网络连接 */
    NETWORK_LINK_WIFI,                  /* WiFi 连接 */
    NETWORK_LINK_ETHERNET,              /* 以太网连接（预留） */
} network_link_t;

#define NETWORK_WIFI_SSID_MAX_LEN      32U      /* SSID 最大长度 */
#define NETWORK_WIFI_PASSWORD_MAX_LEN  64U      /* 密码最大长度 */
#define NETWORK_IPV4_STRING_MAX_LEN    16U      /* IPv4 字符串最大长度 */
#define NETWORK_DEVICE_ID_MAX_LEN      24U      /* 设备 ID 最大长度 */

/* WiFi 配置（SSID + 密码） */
typedef struct {
    char ssid[NETWORK_WIFI_SSID_MAX_LEN + 1U];
    char password[NETWORK_WIFI_PASSWORD_MAX_LEN + 1U];
} network_wifi_config_t;

/* 网络运行时状态 */
typedef struct {
    bool configured;                    /* WiFi 是否已配置 */
    bool sta_connected;                 /* STA 模式是否已连接 */
    bool fallback_ap_active;            /* 回退 AP 模式是否活跃 */
    char device_id[NETWORK_DEVICE_ID_MAX_LEN]; /* 设备唯一标识 */
    char ip[NETWORK_IPV4_STRING_MAX_LEN];      /* 当前 IP 地址 */
    char ssid[NETWORK_WIFI_SSID_MAX_LEN + 1U]; /* 连接的路由器 SSID */
} network_status_t;

/* 初始化网络栈 */
esp_err_t network_service_init(void);
/* 启动网络连接（STA 优先，失败回退 AP） */
esp_err_t network_service_start(void);
/* 暂停网络服务（空闲模式） */
esp_err_t network_service_quiesce(void);
/* 恢复网络服务 */
esp_err_t network_service_resume(void);
/* 设置 WiFi 凭据 */
esp_err_t network_service_set_wifi_config(const network_wifi_config_t *config);
/* 获取 WiFi 凭据 */
esp_err_t network_service_get_wifi_config(network_wifi_config_t *config);
/* 获取网络运行状态 */
esp_err_t network_service_get_status(network_status_t *status);
/* 获取当前活跃链路类型 */
network_link_t network_service_active_link(void);