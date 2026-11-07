/* 网络服务实现
 * 负责所有网络相关功能的实现，包括：
 * - WiFi STA 模式连接路由器（失败自动回退 AP 模式）
 * - HTTP 服务器（MJPEG 流 + OTA + REST API）
 * - TCP 二进制流服务器（基于 stream_protocol 协议）
 * - NTP/HTTP Date 时间同步
 * - 人脸注册/管理命令处理
 * - OTA 固件升级
 */

#include "network_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "face_detection_service.h"
#include "face_recognition_service.h"
#include "media_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_service.h"
#include "media_service.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_service.h"
#include "recording_service.h"
#include "sdkconfig.h"
#include "stream_protocol.h"
#include "storage_service.h"

#define MJPEG_BOUNDARY       "s31frame"
#define MJPEG_TASK_STACK     6144U
#define MJPEG_TASK_PRIORITY  4U
#define MJPEG_MAX_CLIENTS    2U
#define TCP_STREAM_PORT      5000U
#define TCP_SERVER_STACK     4096U
#define TCP_CLIENT_STACK     4096U
#define FACE_ENROLL_STACK    8192U
#define TCP_TASK_PRIORITY    4U
#define TCP_MAX_CLIENTS      2U
#define WIFI_CONNECT_RETRIES 5U
#define WIFI_NVS_NAMESPACE  "camera_net"
#define OTA_RECEIVE_BUFFER  4096U

typedef struct {
    httpd_req_t *request;
} mjpeg_client_t;

typedef struct {
    int socket;
} tcp_client_t;

typedef struct {
    const uint16_t *rgb565;
    uint16_t width;
    uint16_t height;
    uint16_t new_id;
    esp_err_t result;
    TaskHandle_t waiting_task;
} face_enroll_job_t;

static const char *TAG = "network_service";
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static volatile bool s_running;
static volatile bool s_stream_enabled = true;
static bool s_wifi_started;
static network_link_t s_link = NETWORK_LINK_NONE;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_http_server;
static uint32_t s_mjpeg_clients;
static TaskHandle_t s_tcp_server_task;
static volatile uint32_t s_tcp_clients;
static int s_tcp_listen_socket = -1;
static SemaphoreHandle_t s_network_lock;
static network_wifi_config_t s_wifi_config;
static bool s_wifi_configured;
static bool s_sta_connected;
static bool s_fallback_ap_active;
static bool s_saved_lease_valid;
static esp_netif_ip_info_t s_saved_lease;
static uint32_t s_connect_retries;
static char s_device_id[NETWORK_DEVICE_ID_MAX_LEN];
static char s_current_ip[NETWORK_IPV4_STRING_MAX_LEN] = "192.168.4.1";
static volatile bool s_ota_active;
static bool s_http_time_task_started;

typedef struct {
    char date[64];
} http_time_context_t;

/* SNTP 时间同步回调，可在此记录日志或触发后续流程 */
static void time_sync_notification(struct timeval *time_value);

/* 配置 STA 接口的 DNS 服务器（阿里 223.5.5.5 + 腾讯 119.29.29.29） */
static void configure_public_dns(void)
{
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);
    esp_err_t main_ret = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(119, 29, 29, 29);
    esp_err_t backup_ret = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    if (main_ret == ESP_OK && backup_ret == ESP_OK) {
        ESP_LOGI(TAG, "DNS configured: primary=223.5.5.5 backup=119.29.29.29");
    } else {
        ESP_LOGW(TAG, "DNS configuration failed: primary=%s backup=%s",
                 esp_err_to_name(main_ret), esp_err_to_name(backup_ret));
    }
}

/* 系统时间是否有效（2024-01-01 之后） */
static bool system_time_valid(void)
{
    return time(NULL) >= 1704067200; /* 2024-01-01 */
}

/* 公历日期 → 纪元天数偏移（替代标准库 mktime 的浮点回退方案） */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3U : month + 9U;
    const unsigned doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* 从 HTTP Date 头解析时间并设置系统时钟（SNTP 失败时的回退方案） */
static bool set_time_from_http_date(const char *date)
{
    char weekday[4] = {0};
    char month_name[4] = {0};
    char zone[4] = {0};
    int day, year, hour, minute, second;
    if (sscanf(date, "%3s %d %3s %d %d:%d:%d %3s", weekday, &day, month_name,
               &year, &hour, &minute, &second, zone) != 8 || strcasecmp(zone, "GMT") != 0) {
        return false;
    }
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (strcasecmp(month_name, months[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0 || year < 2024 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }
    int64_t epoch = days_from_civil(year, month, (unsigned)day) * 86400LL +
                    hour * 3600LL + minute * 60LL + second;
    struct timeval value = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&value, NULL) != 0) {
        return false;
    }
    time_sync_notification(&value);
    ESP_LOGI(TAG, "time synchronized through HTTP Date fallback");
    return true;
}

/* HTTP 客户端事件回调：在收到 Date 响应头时抓取原始字符串 */
static esp_err_t http_time_event(esp_http_client_event_t *event)
{
    http_time_context_t *context = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != NULL &&
        event->header_value != NULL && strcasecmp(event->header_key, "Date") == 0) {
        strlcpy(context->date, event->header_value, sizeof(context->date));
    }
    return ESP_OK;
}

/* HTTP 时间同步后台任务：向百度/腾讯发送 HEAD 请求获取 Date 头。
 * SNTP 失败时自动启动，同步成功后自行删除。 */
static void http_time_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(8000));
    const char *urls[] = {"http://www.baidu.com/", "http://www.qq.com/"};
    while (!system_time_valid() && s_sta_connected) {
        if (s_ota_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]) && !system_time_valid(); ++i) {
            http_time_context_t context = {0};
            esp_http_client_config_t config = {
                .url = urls[i],
                .method = HTTP_METHOD_HEAD,
                .timeout_ms = 5000,
                .event_handler = http_time_event,
                .user_data = &context,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client != NULL) {
                esp_err_t ret = esp_http_client_perform(client);
                if (ret == ESP_OK && context.date[0] != '\0') {
                    (void)set_time_from_http_date(context.date);
                }
                esp_http_client_cleanup(client);
            }
        }
        if (!system_time_valid()) {
            ESP_LOGW(TAG, "network time unavailable; retrying in 30 seconds");
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
    s_http_time_task_started = false;
    vTaskDelete(NULL);
}

/* SNTP 时间同步成功回调：打印当前系统本地时间 */
static void time_sync_notification(struct timeval *time_value)
{
    time_t now = time_value->tv_sec;
    struct tm local = {0};
    localtime_r(&now, &local);
    ESP_LOGI(TAG, "time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

/* OTA 完成后延迟重启任务（750ms 延迟） */
static void ota_restart_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

/* 交叉核心人脸注册任务：网络服务运行在 CPU0，而人脸识别模型绑定在 CPU1。
 * 因此通过创建任务在 CPU1 执行注册，完成后通知 CPU0 继续。 */
static void face_enroll_task(void *argument)
{
    face_enroll_job_t *job = argument;
    job->result = media_service_enroll_face_upload(job->rgb565, job->width,
                                                    job->height, &job->new_id);
    xTaskNotifyGive(job->waiting_task);
    vTaskDelete(NULL);
}

/* 在 CPU1 上异步执行人脸注册，当前线程阻塞等待结果。
 * 注册成功时 *new_id 返回新分配的人脸 ID。 */
static esp_err_t run_face_enroll_on_vision_core(const uint16_t *rgb565,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 uint16_t *new_id)
{
    face_enroll_job_t job = {
        .rgb565 = rgb565,
        .width = width,
        .height = height,
        .new_id = 0,
        .result = ESP_FAIL,
        .waiting_task = xTaskGetCurrentTaskHandle(),
    };
    BaseType_t created = xTaskCreatePinnedToCore(face_enroll_task, "face_enroll_upload",
                                                 FACE_ENROLL_STACK, &job,
                                                 3, NULL, 1);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    *new_id = job.new_id;
    return job.result;
}

/* 从 NVS 加载 Wi-Fi 配置（SSID/密码 + 上次 DHCP 租约） */
static esp_err_t load_wifi_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "network settings NVS open failed");

    size_t ssid_size = sizeof(s_wifi_config.ssid);
    size_t password_size = sizeof(s_wifi_config.password);
    ret = nvs_get_str(handle, "ssid", s_wifi_config.ssid, &ssid_size);
    if (ret == ESP_OK) {
        ret = nvs_get_str(handle, "password", s_wifi_config.password, &password_size);
    }
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        memset(&s_wifi_config, 0, sizeof(s_wifi_config));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "network settings NVS read failed");

    uint8_t lease_valid = 0;
    ret = nvs_get_u8(handle, "lease_valid", &lease_valid);
    if (ret == ESP_OK && lease_valid != 0) {
        ret = nvs_get_u32(handle, "lease_ip", &s_saved_lease.ip.addr);
        if (ret == ESP_OK) {
            ret = nvs_get_u32(handle, "lease_mask", &s_saved_lease.netmask.addr);
        }
        if (ret == ESP_OK) {
            ret = nvs_get_u32(handle, "lease_gw", &s_saved_lease.gw.addr);
        }
        s_saved_lease_valid = ret == ESP_OK;
    }
    nvs_close(handle);
    s_wifi_configured = s_wifi_config.ssid[0] != '\0';
    return ESP_OK;
}

/* 持久化 Wi-Fi 凭据到 NVS */
static esp_err_t save_wifi_config(const network_wifi_config_t *config)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "network settings NVS open failed");
    esp_err_t ret = nvs_set_str(handle, "ssid", config->ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "password", config->password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "lease_valid", 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

/* 持久化当前 DHCP 租约到 NVS（用于重启后快速恢复 IP） */
static esp_err_t save_ip_lease(const esp_netif_ip_info_t *info)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "IP lease NVS open failed");
    esp_err_t ret = nvs_set_u32(handle, "lease_ip", info->ip.addr);
    if (ret == ESP_OK) {
        ret = nvs_set_u32(handle, "lease_mask", info->netmask.addr);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(handle, "lease_gw", info->gw.addr);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "lease_valid", 1);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

/* 填充 SoftAP 的 Wi-Fi 配置结构体（SSID/密码/信道/认证模式） */
static void fill_ap_config(wifi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strlcpy((char *)config->ap.ssid, CONFIG_S31_CAM_WIFI_AP_SSID,
            sizeof(config->ap.ssid));
    strlcpy((char *)config->ap.password, CONFIG_S31_CAM_WIFI_AP_PASSWORD,
            sizeof(config->ap.password));
    config->ap.ssid_len = strlen(CONFIG_S31_CAM_WIFI_AP_SSID);
    config->ap.channel = CONFIG_S31_CAM_WIFI_AP_CHANNEL;
    config->ap.max_connection = 4;
    config->ap.authmode = strlen(CONFIG_S31_CAM_WIFI_AP_PASSWORD) == 0
                              ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
}

/* 启用 STA 模式和 DHCP（由路由器分配 DNS 服务器） */
static esp_err_t configure_sta(const network_wifi_config_t *config)
{
    /* Always use DHCP. Restoring only the previous IP/mask/gateway omitted the
     * DNS lease, leaving the device able to reach the LAN but unable to resolve
     * NTP and HTTP time hostnames. The router normally assigns the same address
     * again while also supplying valid DNS servers. */
    (void)esp_netif_dhcpc_start(s_sta_netif);
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

/* 启动 SoftAP 回退热点：STA 连接失败时提供 192.168.4.1 管理入口 */
static void enable_fallback_ap(void)
{
    wifi_config_t ap_config;
    fill_ap_config(&ap_config);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
        esp_wifi_set_config(WIFI_IF_AP, &ap_config) == ESP_OK) {
        xSemaphoreTake(s_network_lock, portMAX_DELAY);
        s_fallback_ap_active = true;
        strlcpy(s_current_ip, "192.168.4.1", sizeof(s_current_ip));
        xSemaphoreGive(s_network_lock);
        ESP_LOGW(TAG, "router connection failed; setup hotspot restored: %s",
                 CONFIG_S31_CAM_WIFI_AP_SSID);
    }
}

/* Wi-Fi 事件回调处理：
 * STA_START→连接、STA_DISCONNECTED→重试/回退、STA_CONNECTED→DHCP/DNS/时间同步 */
static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && s_wifi_configured) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
               s_wifi_configured) {
        xSemaphoreTake(s_network_lock, portMAX_DELAY);
        s_sta_connected = false;
        bool retry = s_connect_retries++ < WIFI_CONNECT_RETRIES;
        xSemaphoreGive(s_network_lock);
        if (retry) {
            esp_wifi_connect();
        } else {
            enable_fallback_ap();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        esp_err_t save_ret = save_ip_lease(&event->ip_info);
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "IP lease save failed: %s", esp_err_to_name(save_ret));
        }
        xSemaphoreTake(s_network_lock, portMAX_DELAY);
        s_sta_connected = true;
        s_fallback_ap_active = false;
        s_saved_lease = event->ip_info;
        s_saved_lease_valid = true;
        s_connect_retries = 0;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(s_network_lock);
        ESP_LOGI(TAG, "router connected: device=%s IP=" IPSTR,
                 s_device_id, IP2STR(&event->ip_info.ip));
        configure_public_dns();
        if (!esp_sntp_enabled()) {
            setenv("TZ", "CST-8", 1);
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            /* The project config has one SNTP server slot. Aliyun is more
             * consistently reachable from the device's deployment network. */
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_set_time_sync_notification_cb(time_sync_notification);
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP time synchronization started");
        }
        if (!system_time_valid() && !s_http_time_task_started) {
            s_http_time_task_started =
                xTaskCreate(http_time_task, "http_time", 4096, NULL, 2, NULL) == pdPASS;
        }
    }
}

/* 循环调用 send() 确保完整发送所有数据（处理 TCP 流的部分写入） */
static bool send_all(int socket, const void *data, size_t size)
{
    const uint8_t *cursor = data;
    while (size > 0) {
        int sent = send(socket, cursor, size, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        size -= (size_t)sent;
    }
    return true;
}

/* 构建协议帧头 + 可选 payload，通过 TCP socket 发送。
 * 帧头使用 stream_protocol 编码为大端序 32 字节，含 CRC32 校验。 */
static bool send_protocol_message(int socket, stream_message_type_t type, uint32_t sequence,
                                  int64_t timestamp_us, uint16_t width, uint16_t height,
                                  const void *payload, size_t payload_size)
{
    stream_frame_header_t header = {
        .magic = STREAM_PROTOCOL_MAGIC,
        .version = STREAM_PROTOCOL_VERSION,
        .type = type,
        .sequence = sequence,
        .timestamp_us = timestamp_us,
        .width = width,
        .height = height,
        .payload_length = payload_size,
    };
    uint8_t wire_header[STREAM_PROTOCOL_HEADER_SIZE];
    if (stream_protocol_encode_header(&header, wire_header) != ESP_OK) {
        return false;
    }
    return send_all(socket, wire_header, sizeof(wire_header)) &&
           (payload_size == 0 || send_all(socket, payload, payload_size));
}

/* 写入大端 uint16（网络字节序） */
static void write_u16_be(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8);
    target[1] = (uint8_t)value;
}

/* 写入大端 uint32（网络字节序） */
static void write_u32_be(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t)value;
}

/* 将人脸检测+识别结果打包为协议帧发送到 TCP 客户端
 * payload 格式: [4B source_sequence][1B count][每脸18B: x/y/w/h/score/rid/conf] */
static bool send_face_results(int socket, const face_detection_results_t *results)
{
    uint8_t payload[8U + FACE_DETECTION_MAX_RESULTS * 18U] = {0};
    write_u32_be(payload, results->source_sequence);
    payload[4] = results->count;

    media_face_recognition_t recog[MEDIA_FACE_RECOGNITION_MAX] = {};
    uint8_t recog_count = 0;
    (void)media_service_get_recognition_labels(recog, &recog_count);

    for (uint8_t i = 0; i < results->count; ++i) {
        const face_detection_box_t *box = &results->boxes[i];
        uint8_t *wire = payload + 8U + i * 18U;
        write_u16_be(wire, box->x);
        write_u16_be(wire + 2, box->y);
        write_u16_be(wire + 4, box->width);
        write_u16_be(wire + 6, box->height);
        write_u16_be(wire + 8, box->score_x10000);
        wire[10] = 0;
        wire[11] = 0;

        uint16_t rid = 0;
        float rconf = 0.0f;
        if (i < recog_count) {
            rid = recog[i].id;
            rconf = recog[i].confidence;
            write_u16_be(wire + 12, rid);
            uint32_t conf_bits;
            memcpy(&conf_bits, &rconf, sizeof(conf_bits));
            write_u32_be(wire + 14, conf_bits);
        } else {
            write_u16_be(wire + 12, 0);
            write_u32_be(wire + 14, 0);
        }
        ESP_LOGI(TAG, "TX face #%u: box(%u,%u %ux%u) id=%u conf=%.3f",
                 i, box->x, box->y, box->width, box->height,
                 rid, (double)rconf);
    }
    return send_protocol_message(socket, STREAM_MSG_FACE_RESULTS, results->generation,
                                 esp_timer_get_time(), results->source_width,
                                 results->source_height, payload,
                                 8U + results->count * 18U);
}

/* TCP 客户端会话任务（每个连接一个任务）：
 * 1. HELLO 握手 → 2. 主循环（推帧/推人脸/收命令）→ 3. 断开清理
 * 使用 select() 非阻塞轮询命令消息，帧推送基于信号量通知。 */
static void tcp_client_task(void *argument)
{
    tcp_client_t *client = argument;
    const int socket = client->socket;
    free(client);

    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    media_mode_t mode = { .width = 640, .height = 480 };
    (void)media_service_get_mode(&mode);

    /* 发送 HELLO 握手帧，告知设备 ID、协议版本和分辨率 */
    char hello[128];
    snprintf(hello, sizeof(hello),
             "ESP32-S31 Camera;id=%s;protocol=1;format=JPEG;size=%ux%u",
             s_device_id, mode.width, mode.height);
    bool connected = send_protocol_message(socket, STREAM_MSG_HELLO, 0, 0,
                                            mode.width, mode.height,
                                            hello, strlen(hello));
    uint32_t last_sequence = 0;
    uint32_t last_face_generation = UINT32_MAX;

    /* 主循环：推送 JPEG 帧 + 人脸结果 + 处理上位机命令 */
    while (s_running && connected) {
        if (!s_stream_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        jpeg_frame_t frame = { ._slot = UINT8_MAX };
        if (!jpeg_service_acquire_latest(&frame)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (frame.sequence == last_sequence) {
            jpeg_service_release(&frame);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 发送 JPEG 帧 */
        connected = send_protocol_message(socket, STREAM_MSG_JPEG_FRAME, frame.sequence,
                                          frame.timestamp_us, frame.width, frame.height,
                                          frame.data, frame.size);
        last_sequence = frame.sequence;
        jpeg_service_release(&frame);

        /* 如果检测结果有更新，追加发送人脸框数据 */
        face_detection_results_t faces = {0};
        if (connected && face_detection_service_get_latest(&faces) &&
            faces.generation != last_face_generation) {
            connected = send_face_results(socket, &faces);
            last_face_generation = faces.generation;
        }

        /* 非阻塞轮询，接收上位机控制命令 */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket, &readfds);
        struct timeval sel_timeout = { .tv_sec = 0, .tv_usec = 5000 };
        if (select(socket + 1, &readfds, NULL, NULL, &sel_timeout) > 0 &&
            FD_ISSET(socket, &readfds)) {
            static uint32_t s_cmd_count = 0;
            stream_frame_header_t hdr;
            esp_err_t header_ret = stream_protocol_read_header(socket, &hdr, 250000, 2000);
            if (header_ret != ESP_OK) {
                if (header_ret == ESP_FAIL) connected = false;
                continue;
            }
            s_cmd_count++;
            ESP_LOGI(TAG, "cmd #%lu: type=%u payload_len=%lu",
                     (unsigned long)s_cmd_count,
                     (unsigned int)hdr.type,
                     (unsigned long)hdr.payload_length);

            /* 读取命令 payload */
            uint8_t *payload = NULL;
            if (hdr.payload_length > 0) {
                payload = (uint8_t *)malloc(hdr.payload_length);
                if (payload == NULL) continue;
                size_t received = 0;
                while (received < hdr.payload_length) {
                    ssize_t n = recv(socket, payload + received,
                                     hdr.payload_length - received, 0);
                    if (n <= 0) {
                        free(payload);
                        payload = NULL;
                        if (n < 0) connected = false;
                        break;
                    }
                    received += (size_t)n;
                }
                if (payload == NULL) continue;
            }

            /* 根据命令类型分发处理 */
            switch (hdr.type) {
            case STREAM_MSG_FACE_ENROLL:
                ESP_LOGI(TAG, "FACE_ENROLL received, payload_len=%lu", (unsigned long)hdr.payload_length);
                if (payload != NULL && hdr.payload_length >= 1) {
                    /* payload[0]==1: 上传注册模式，含姓名+RGB565图像 */
                    if (payload[0] == 1 && hdr.payload_length >= 7) {
                        uint8_t name_len = payload[1];
                        if (name_len > 0 && (size_t)(7 + name_len) <= hdr.payload_length) {
                            uint16_t crop_w = ((uint16_t)payload[2 + name_len] << 8) | payload[3 + name_len];
                            uint16_t crop_h = ((uint16_t)payload[4 + name_len] << 8) | payload[5 + name_len];
                            const uint16_t *rgb565 = (const uint16_t *)(payload + 6 + name_len);
                            uint16_t new_id = 0;
                            esp_err_t err = run_face_enroll_on_vision_core(rgb565, crop_w,
                                                                           crop_h, &new_id);
                            if (err == ESP_OK) {
                                ESP_LOGI(TAG, "face enrolled (upload) as ID %u", new_id);
                                char name_buf[32];
                                size_t copy_len = name_len < (sizeof(name_buf) - 1) ? name_len : (sizeof(name_buf) - 1);
                                memcpy(name_buf, payload + 2, copy_len);
                                name_buf[copy_len] = '\0';
                                media_service_set_face_name(new_id, name_buf);
                            } else {
                                ESP_LOGW(TAG, "face upload enrollment failed: %s", esp_err_to_name(err));
                            }
                            /* 回复新分配的 ID */
                            uint8_t ack[2];
                            ack[0] = (uint8_t)(new_id >> 8);
                            ack[1] = (uint8_t)new_id;
                            send_protocol_message(socket, STREAM_MSG_FACE_ENROLL_ACK, 0, 0, 0, 0, ack, 2);
                        }
                    } else {
                        /* payload[0]!=1: 本地注册模式，仅传检测框索引 */
                        media_service_enroll_face(payload[0]);
                    }
                }
                break;
            case STREAM_MSG_FACE_DELETE:
                if (payload != NULL && hdr.payload_length >= 2) {
                    uint16_t del_id = ((uint16_t)payload[0] << 8) | payload[1];
                    face_recognition_service_delete(del_id);
                    ESP_LOGI(TAG, "face ID %u deleted by host", del_id);
                }
                break;
            case STREAM_MSG_FACE_CLEAR:
                face_recognition_service_clear();
                media_service_clear_face_names();
                ESP_LOGI(TAG, "face database cleared by host");
                break;
            case STREAM_MSG_FACE_LIST: {
                /* 返回 JSON 格式的人脸名单 {id, name} 列表 */
                media_face_list_entry_t list[MEDIA_FACE_LIST_MAX];
                uint8_t list_count = 0;
                media_service_get_face_list(list, &list_count);
                size_t reply_len = 2;
                for (uint8_t i = 0; i < list_count; i++) {
                    reply_len += 3 + strlen(list[i].name);
                }
                uint8_t *reply = malloc(reply_len);
                if (reply != NULL) {
                    reply[0] = (uint8_t)(list_count >> 8);
                    reply[1] = (uint8_t)list_count;
                    size_t offset = 2;
                    for (uint8_t i = 0; i < list_count; i++) {
                        reply[offset++] = (uint8_t)(list[i].id >> 8);
                        reply[offset++] = (uint8_t)list[i].id;
                        uint8_t nlen = (uint8_t)strlen(list[i].name);
                        reply[offset++] = nlen;
                        memcpy(reply + offset, list[i].name, nlen);
                        offset += nlen;
                    }
                    send_protocol_message(socket, STREAM_MSG_FACE_LIST_REPLY, 0, 0, 0, 0, reply, reply_len);
                    free(reply);
                }
                ESP_LOGI(TAG, "face list sent: %u entries", list_count);
                break;
            }
            case STREAM_MSG_FACE_THRESHOLD_SET:
                if (payload != NULL && hdr.payload_length == 2) {
                    uint16_t threshold_x1000 = ((uint16_t)payload[0] << 8) | payload[1];
                    if (threshold_x1000 <= 1000) {
                        face_recognition_service_threshold_set((float)threshold_x1000 / 1000.0f);
                        uint8_t ack[2] = {
                            (uint8_t)(threshold_x1000 >> 8),
                            (uint8_t)threshold_x1000,
                        };
                        send_protocol_message(socket, STREAM_MSG_FACE_THRESHOLD_ACK,
                                              0, 0, 0, 0, ack, sizeof(ack));
                        ESP_LOGI(TAG, "face recognition threshold set to %.3f",
                                 (double)face_recognition_service_threshold_get());
                    }
                }
                break;
            default:
                break;
            }
            free(payload);
        }
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    taskENTER_CRITICAL(&s_client_lock);
    --s_tcp_clients;
    taskEXIT_CRITICAL(&s_client_lock);
    ESP_LOGI(TAG, "TCP stream client disconnected");
    vTaskDelete(NULL);
}

/* TCP 二进制流服务器任务（端口 5000）：
 * 在 CPU0 上监听，接受最多 TCP_MAX_CLIENTS (2) 个并发连接，
 * 每个客户端分配独立任务推送 JPEG 帧并处理控制命令。 */
static void tcp_server_task(void *argument)
{
    (void)argument;
    int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_socket < 0) {
        ESP_LOGE(TAG, "TCP socket creation failed: errno=%d", errno);
        goto exit;
    }
    int reuse = 1;  /* SO_REUSEADDR 允许快速重启后立即绑定端口 */
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_socket, TCP_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "TCP port %u setup failed: errno=%d", TCP_STREAM_PORT, errno);
        close(listen_socket);
        goto exit;
    }
    s_tcp_listen_socket = listen_socket;
    ESP_LOGI(TAG, "TCP JPEG stream listening on port %u", TCP_STREAM_PORT);

    while (s_running) {
        int client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket < 0) {
            if (s_running) {
                ESP_LOGW(TAG, "TCP accept failed: errno=%d", errno);
            }
            continue;
        }
        taskENTER_CRITICAL(&s_client_lock);
        bool accepted = s_tcp_clients < TCP_MAX_CLIENTS;
        if (accepted) {
            ++s_tcp_clients;
        }
        taskEXIT_CRITICAL(&s_client_lock);
        if (!accepted) {
            close(client_socket);
            continue;
        }
        tcp_client_t *client = calloc(1, sizeof(*client));
        if (client == NULL) {
            close(client_socket);
            taskENTER_CRITICAL(&s_client_lock);
            --s_tcp_clients;
            taskEXIT_CRITICAL(&s_client_lock);
            continue;
        }
        client->socket = client_socket;
        if (xTaskCreatePinnedToCore(tcp_client_task, "tcp_stream_client", TCP_CLIENT_STACK,
                                    client, TCP_TASK_PRIORITY, NULL, 0) != pdPASS) {
            free(client);
            close(client_socket);
            taskENTER_CRITICAL(&s_client_lock);
            --s_tcp_clients;
            taskEXIT_CRITICAL(&s_client_lock);
            continue;
        }
        ESP_LOGI(TAG, "TCP stream client connected");
    }

    close(listen_socket);
    s_tcp_listen_socket = -1;
exit:
    s_tcp_server_task = NULL;
    vTaskDelete(NULL);
}

/* 创建 TCP 服务器监听任务（绑定 CPU0） */
static esp_err_t start_tcp_server(void)
{
    if (s_tcp_server_task != NULL) {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreatePinnedToCore(tcp_server_task, "tcp_stream_server",
                                                 TCP_SERVER_STACK, NULL, TCP_TASK_PRIORITY,
                                                 &s_tcp_server_task, 0);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* 发送 JSON 响应并设置 CORS 头 */
static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(request, json);
}

/* HTTP 根路径：返回内嵌 MJPEG 播放器的 HTML 页面 */
static esp_err_t root_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width'>"
        "<title>ESP32-S31 Camera</title></head>"
        "<body style='margin:0;background:#101820;color:#e2e8f0;font-family:sans-serif;text-align:center'>"
        "<h2>ESP32-S31 Camera</h2>"
        "<img src='/stream.mjpeg' style='max-width:100%;height:auto;border:2px solid #38bdf8'>"
        "<p><a style='color:#38bdf8' href='/api/v1/status'>Device status</a></p>"
        "</body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

/* /api/v1/status：返回设备运行状态 JSON
 * 包含设备 ID、Wi-Fi 状态、摄像头模式、帧率、人脸检测统计等。 */
static esp_err_t status_handler(httpd_req_t *request)
{
    network_status_t status = {0};
    network_service_get_status(&status);
    media_mode_t mode = {0};
    (void)media_service_get_mode(&mode);
    face_detection_results_t faces = {0};
    (void)face_detection_service_get_latest(&faces);
    char response[896];
    snprintf(response, sizeof(response),
             "{\"ready\":true,\"device_id\":\"%s\",\"link\":\"%s\",\"ip\":\"%s\"," 
             "\"ssid\":\"%s\",\"fallback_ap\":%s,"
             "\"media_running\":%s,\"streaming\":%s,\"width\":%u,\"height\":%u,"
             "\"rgb_frames\":%lu,\"jpeg_frames\":%lu,"
             "\"jpeg_dropped\":%lu,\"mjpeg_clients\":%lu,\"tcp_clients\":%lu,"
             "\"face_detection\":%s,\"face_ready\":%s,\"face_count\":%u,"
             "\"face_inference_ms\":%lu,\"face_dropped\":%lu,"
             "\"sd_mounted\":%s,\"recording\":%s,\"recorded_frames\":%lu}",
             status.device_id, status.sta_connected ? "wifi_sta" : "wifi_ap", status.ip,
             status.ssid, status.fallback_ap_active ? "true" : "false",
             media_service_is_running() ? "true" : "false",
             s_stream_enabled ? "true" : "false",
             mode.width, mode.height,
             (unsigned long)media_service_frame_count(),
             (unsigned long)jpeg_service_encoded_count(),
             (unsigned long)jpeg_service_dropped_count(),
             (unsigned long)s_mjpeg_clients, (unsigned long)s_tcp_clients,
             face_detection_service_is_enabled() ? "true" : "false",
             face_detection_service_is_ready() ? "true" : "false", faces.count,
             (unsigned long)face_detection_service_inference_ms(),
             (unsigned long)face_detection_service_dropped_frames(),
             storage_service_sd_mounted() ? "true" : "false",
             recording_service_is_running() ? "true" : "false",
             (unsigned long)recording_service_frame_count());
    return send_json(request, response);
}

/* /api/v1/capabilities：返回设备能力描述 JSON
 * 包含摄像头规格、分辨率表、协议支持、人脸功能、录像等完整能力清单。 */
static esp_err_t capabilities_handler(httpd_req_t *request)
{
    return send_json(request,
                     "{\"camera\":\"OV3660\",\"width\":640,\"height\":480,"
                     "\"resolutions\":[{\"width\":640,\"height\":480,\"fps\":10},"
                     "{\"width\":240,\"height\":240,\"fps\":24}],"
                     "\"formats\":[\"RGB565\",\"JPEG\"],\"mjpeg\":true,"
                     "\"tcp_stream\":true,\"tcp_port\":5000,"
                     "\"face_detection\":true,\"face_results_tcp\":true,"
                     "\"snapshot\":false,\"ota\":true,"
                     "\"sd_recording\":true,\"recording_format\":\"MJPEG AVI\","
                     "\"ota_endpoint\":\"/api/v1/ota\"}");
}

/* 验证 OTA 请求的 X-OTA-Token 头是否匹配预设令牌 */
static bool ota_token_valid(httpd_req_t *request)
{
    const size_t length = httpd_req_get_hdr_value_len(request, "X-OTA-Token");
    if (length == 0 || length >= 96) {
        return false;
    }
    char token[96];
    return httpd_req_get_hdr_value_str(request, "X-OTA-Token", token,
                                       sizeof(token)) == ESP_OK &&
           strcmp(token, CONFIG_S31_CAM_OTA_TOKEN) == 0;
}

/* OTA 流程中暂停/恢复各项服务的辅助：
 * 保存暂停前的运行状态，写入完成后恢复。 */
static void ota_restore_services(bool media_was_running, bool detection_was_enabled,
                                 bool recognition_was_enabled)
{
    if (media_was_running) {
        (void)media_service_start();
    }
    if (detection_was_enabled) {
        (void)face_detection_service_set_enabled(true);
    }
    if (recognition_was_enabled) {
        (void)face_recognition_service_set_enabled(true);
    }
    s_stream_enabled = true;
}

/* /api/v1/ota (POST)：接收固件二进制镜像
 * 需要 X-OTA-Token 鉴权头，验证通过后进入以下流程：
 * 1. 暂停所有非必要服务（释放内存和 CPU）
 * 2. 调用 ota_service begin/write/finish
 * 3. 成功后返回 202 并计划 2 秒后重启 */
static esp_err_t ota_handler(httpd_req_t *request)
{
    if (!ota_token_valid(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"invalid OTA token\"}");
    }
    if (request->content_len <= 0 || s_ota_active) {
        httpd_resp_set_status(request, s_ota_active ? "409 Conflict" : "400 Bad Request");
        return send_json(request, s_ota_active ? "{\"error\":\"OTA already active\"}" :
                                                "{\"error\":\"empty firmware image\"}");
    }

    s_ota_active = true;
    const bool media_was_running = media_service_is_running();
    const bool detection_was_enabled = face_detection_service_is_enabled();
    const bool recognition_was_enabled = face_recognition_service_is_enabled();
    const bool recording_was_running = recording_service_is_running();
    s_stream_enabled = false;
    (void)face_detection_service_set_enabled(false);
    (void)face_recognition_service_set_enabled(false);
    esp_err_t ret = ESP_OK;
    if (ret == ESP_OK && recording_was_running) {
        ret = recording_service_stop();
    }
    if (ret == ESP_OK && media_was_running) {
        ret = media_service_stop();
    }
    if (ret == ESP_OK) {
        ret = ota_service_begin((size_t)request->content_len);
    }

    uint8_t *buffer = ret == ESP_OK ? malloc(OTA_RECEIVE_BUFFER) : NULL;
    if (ret == ESP_OK && buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
    }
    size_t received_total = 0;
    while (ret == ESP_OK && received_total < (size_t)request->content_len) {
        const size_t remaining = (size_t)request->content_len - received_total;
        const size_t wanted = remaining < OTA_RECEIVE_BUFFER ? remaining : OTA_RECEIVE_BUFFER;
        int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        if (received <= 0) {
            ret = ESP_FAIL;
            break;
        }
        if (received_total == 0 && buffer[0] != 0xE9) {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        ret = ota_service_write(buffer, (size_t)received);
        received_total += (size_t)received;
    }
    free(buffer);

    if (ret == ESP_OK && received_total == (size_t)request->content_len) {
        ret = ota_service_finish();
    } else {
        ota_service_abort();
    }

    if (ret != ESP_OK) {
        ota_service_abort();
        ota_restore_services(media_was_running, detection_was_enabled,
                             recognition_was_enabled);
        if (recording_was_running && storage_service_sd_mounted()) {
            (void)recording_service_start();
        }
        s_ota_active = false;
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"error\":\"OTA failed\",\"code\":\"%s\"}", esp_err_to_name(ret));
        httpd_resp_set_status(request, "500 Internal Server Error");
        ESP_LOGE(TAG, "OTA failed after %u bytes: %s", (unsigned)received_total,
                 esp_err_to_name(ret));
        return send_json(request, response);
    }

    ESP_LOGI(TAG, "OTA image accepted: %u bytes; rebooting", (unsigned)received_total);
    esp_err_t response_ret = send_json(request, "{\"ok\":true,\"rebooting\":true}");
    if (xTaskCreate(ota_restart_task, "ota_restart", 2048, NULL, 3, NULL) != pdPASS) {
        esp_restart();
    }
    return response_ret;
}

/* /api/v1/camera/mode (POST)：通过查询参数切换摄像头分辨率。
 * 示例：POST /api/v1/camera/mode?width=240&height=240
 * 切换期间暂停流，切换完成后自动恢复。 */
static esp_err_t camera_mode_handler(httpd_req_t *request)
{
    char query[96];
    char width_text[12];
    char height_text[12];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "width", width_text, sizeof(width_text)) != ESP_OK ||
        httpd_query_key_value(query, "height", height_text, sizeof(height_text)) != ESP_OK) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"width and height query parameters are required\"}");
    }

    char *width_end = NULL;
    char *height_end = NULL;
    unsigned long width = strtoul(width_text, &width_end, 10);
    unsigned long height = strtoul(height_text, &height_end, 10);
    if (*width_text == '\0' || *height_text == '\0' || *width_end != '\0' ||
        *height_end != '\0' || width > UINT16_MAX || height > UINT16_MAX) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"invalid width or height\"}");
    }

    media_mode_t mode = {
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .format = MEDIA_PIXEL_FORMAT_RGB565,
        .jpeg_quality = 80,
    };
    esp_err_t ret = media_service_set_mode(&mode);
    if (ret != ESP_OK) {
        httpd_resp_set_status(request, ret == ESP_ERR_NOT_SUPPORTED
                                          ? "400 Bad Request" : "500 Internal Server Error");
        char error[128];
        snprintf(error, sizeof(error),
                 "{\"error\":\"mode switch failed\",\"code\":\"%s\"}",
                 esp_err_to_name(ret));
        return send_json(request, error);
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"width\":%u,\"height\":%u,\"streaming\":%s}",
             mode.width, mode.height, s_stream_enabled ? "true" : "false");
    return send_json(request, response);
}

/* /api/v1/stream/start (POST)：启用到 TCP 上位机客户端的流推送 */
static esp_err_t stream_start_handler(httpd_req_t *request)
{
    s_stream_enabled = true;
    return send_json(request, "{\"stream\":\"started\"}");
}

/* /api/v1/stream/stop (POST)：暂停到 TCP 上位机客户端的流推送 */
static esp_err_t stream_stop_handler(httpd_req_t *request)
{
    s_stream_enabled = false;
    return send_json(request, "{\"streaming\":false}");
}

/* MJPEG 异步发送任务：循环获取最新 JPEG 帧并推送到 HTTP 客户端。
 * 通过 sequence 去重避免重复发送同一帧，遇到连接错误时退出。 */
static void mjpeg_client_task(void *argument)
{
    mjpeg_client_t *client = argument;
    httpd_req_t *request = client->request;
    free(client);

    httpd_resp_set_type(request, "multipart/x-mixed-replace;boundary=" MJPEG_BOUNDARY);
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");

    uint32_t last_sequence = 0;
    while (s_running) {
        if (!s_stream_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        jpeg_frame_t frame = { ._slot = UINT8_MAX };
        if (!jpeg_service_acquire_latest(&frame)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (frame.sequence == last_sequence) {
            jpeg_service_release(&frame);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        char header[160];
        int header_length = snprintf(header, sizeof(header),
                                     "\r\n--" MJPEG_BOUNDARY "\r\n"
                                     "Content-Type: image/jpeg\r\n"
                                     "Content-Length: %u\r\n"
                                     "X-Sequence: %lu\r\n\r\n",
                                     (unsigned)frame.size, (unsigned long)frame.sequence);
        esp_err_t ret = (header_length > 0)
                            ? httpd_resp_send_chunk(request, header, header_length)
                            : ESP_FAIL;
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(request, (const char *)frame.data, frame.size);
        }
        last_sequence = frame.sequence;
        jpeg_service_release(&frame);
        if (ret != ESP_OK) {
            break;
        }
    }

    httpd_req_async_handler_complete(request);
    taskENTER_CRITICAL(&s_client_lock);
    --s_mjpeg_clients;
    taskEXIT_CRITICAL(&s_client_lock);
    ESP_LOGI(TAG, "MJPEG client disconnected");
    vTaskDelete(NULL);
}

/* /stream.mjpeg：MJPEG over HTTP 多部分流
 * 将 HTTP 请求转为异步模式，创建独立任务向客户端
 * 持续推送 multipart/x-mixed-replace 格式的 JPEG 帧。
 * 最多允许 MJPEG_MAX_CLIENTS (3) 个并发客户端。 */
static esp_err_t mjpeg_handler(httpd_req_t *request)
{
    taskENTER_CRITICAL(&s_client_lock);
    bool accepted = s_mjpeg_clients < MJPEG_MAX_CLIENTS;
    if (accepted) {
        ++s_mjpeg_clients;
    }
    taskEXIT_CRITICAL(&s_client_lock);
    if (!accepted) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "MJPEG client limit reached");
    }

    httpd_req_t *async_request = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(request, &async_request);
    if (ret != ESP_OK) {
        goto fail;
    }
    mjpeg_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        httpd_req_async_handler_complete(async_request);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    client->request = async_request;
    if (xTaskCreatePinnedToCore(mjpeg_client_task, "mjpeg_client", MJPEG_TASK_STACK,
                                client, MJPEG_TASK_PRIORITY, NULL, 0) != pdPASS) {
        free(client);
        httpd_req_async_handler_complete(async_request);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "MJPEG client connected");
    return ESP_OK;

fail:
    taskENTER_CRITICAL(&s_client_lock);
    --s_mjpeg_clients;
    taskEXIT_CRITICAL(&s_client_lock);
    return ret;
}

/* 启动 HTTP 服务器（端口 80），注册 8 条 REST API 路由 */
static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 9;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "HTTP server start failed");

    /* HTTP REST API 路由表 */
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },               /* 首页含 MJPEG 播放器 */
        { .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_handler }, /* JSON 状态查询 */
        { .uri = "/api/v1/capabilities", .method = HTTP_GET, .handler = capabilities_handler }, /* 设备能力 */
        { .uri = "/api/v1/stream/start", .method = HTTP_POST, .handler = stream_start_handler },
        { .uri = "/api/v1/stream/stop", .method = HTTP_POST, .handler = stream_stop_handler },
        { .uri = "/api/v1/camera/mode", .method = HTTP_POST, .handler = camera_mode_handler }, /* 分辨率切换 */
        { .uri = "/api/v1/ota", .method = HTTP_POST, .handler = ota_handler },      /* OTA 固件上传 */
        { .uri = "/stream.mjpeg", .method = HTTP_GET, .handler = mjpeg_handler },    /* MJPEG 视频流 */
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t ret = httpd_register_uri_handler(s_http_server, &routes[i]);
        if (ret != ESP_OK) {
            httpd_stop(s_http_server);
            s_http_server = NULL;
            return ret;
        }
    }
    network_status_t status = {0};
    network_service_get_status(&status);
    ESP_LOGI(TAG, "HTTP camera service: http://%s/", status.ip);
    return ESP_OK;
}

/* 初始化 Wi-Fi 栈：NVS、网络接口、事件循环、STA/AP 配置。
 * 已配置凭据时进入 STA 模式连接路由器，否则启动 SoftAP 设置热点。 */
static esp_err_t initialize_wifi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS recovery erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS initialization failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "network stack initialization failed");
    ret = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, TAG,
                        "default event loop creation failed");

    s_network_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_network_lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "network mutex allocation failed");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "base MAC read failed");
    snprintf(s_device_id, sizeof(s_device_id), "S31CAM-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(load_wifi_config(), TAG, "saved Wi-Fi settings load failed");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_ap_netif != NULL && s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG,
                        "Wi-Fi netif creation failed");
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi RAM storage setup failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "IP event registration failed");

    if (s_wifi_configured) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi STA mode setup failed");
        ESP_RETURN_ON_ERROR(configure_sta(&s_wifi_config), TAG, "saved Wi-Fi setup failed");
        strlcpy(s_current_ip, "DHCP pending", sizeof(s_current_ip));
    } else {
        wifi_config_t ap_config;
        fill_ap_config(&ap_config);
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Wi-Fi AP mode setup failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                            "Wi-Fi AP configuration failed");
        s_fallback_ap_active = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi AP start failed");
    s_wifi_started = true;
    if (s_wifi_configured) {
        ESP_LOGI(TAG, "connecting to router SSID=%s; waiting for DHCP and DNS device=%s",
                 s_wifi_config.ssid, s_device_id);
    } else {
        ESP_LOGI(TAG, "setup hotspot ready: SSID=%s device=%s",
                 CONFIG_S31_CAM_WIFI_AP_SSID, s_device_id);
    }
    return ESP_OK;
}

/* [网络服务 初始化] 初始化 Wi-Fi 栈（STA 模式或 SoftAP 回退） */
esp_err_t network_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(initialize_wifi(), TAG, "Wi-Fi service initialization failed");
    s_initialized = true;
    ESP_LOGI(TAG, "network initialized; mode=%s",
             s_wifi_configured ? "router STA" : "setup SoftAP");
    return ESP_OK;
}

/* [网络服务 启动] 启动 HTTP 服务器和 TCP 流服务器 */
esp_err_t network_service_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_wifi_started, ESP_ERR_INVALID_STATE, TAG,
                        "network service is not initialized");
    s_running = true;
    s_stream_enabled = true;
    esp_err_t ret = start_http_server();
    if (ret != ESP_OK) {
        s_running = false;
        return ret;
    }
    ret = start_tcp_server();
    if (ret != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        s_running = false;
        return ret;
    }
    s_link = NETWORK_LINK_WIFI;
    return ESP_OK;
}

/* [网络服务 静默] 停止 TCP/HTTP 服务，等待客户端断开（OTA 前使用） */
esp_err_t network_service_quiesce(void)
{
    s_running = false;
    if (s_tcp_listen_socket >= 0) {
        shutdown(s_tcp_listen_socket, SHUT_RDWR);
    }
    for (uint32_t waited_ms = 0; s_mjpeg_clients > 0 && waited_ms < 2000; waited_ms += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (uint32_t waited_ms = 0;
         (s_tcp_server_task != NULL || s_tcp_clients > 0) && waited_ms < 2000; waited_ms += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    ESP_LOGI(TAG, "application network services quiesced");
    return ESP_OK;
}

/* [网络服务 恢复] 重新启动网络服务（OTA 完成后调用） */
esp_err_t network_service_resume(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "network service is not initialized");
    return network_service_start();
}

network_link_t network_service_active_link(void)
{
    return s_running ? s_link : NETWORK_LINK_NONE;
}

/* [网络服务 读取Wi-Fi配置] 获取当前保存的Wi-Fi凭据拷贝 */
esp_err_t network_service_get_wifi_config(network_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(s_network_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "network service is not initialized");
    xSemaphoreTake(s_network_lock, portMAX_DELAY);
    *config = s_wifi_config;
    xSemaphoreGive(s_network_lock);
    return ESP_OK;
}

/* [网络服务 状态] 获取当前网络连接状态快照 */
esp_err_t network_service_get_status(network_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_network_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "network service is not initialized");
    memset(status, 0, sizeof(*status));
    xSemaphoreTake(s_network_lock, portMAX_DELAY);
    status->configured = s_wifi_configured;
    status->sta_connected = s_sta_connected;
    status->fallback_ap_active = s_fallback_ap_active;
    strlcpy(status->device_id, s_device_id, sizeof(status->device_id));
    strlcpy(status->ip, s_current_ip, sizeof(status->ip));
    strlcpy(status->ssid, s_wifi_config.ssid, sizeof(status->ssid));
    xSemaphoreGive(s_network_lock);
    return ESP_OK;
}

/* [网络服务 设置Wi-Fi] 保存凭据并热切换到新 SSID 连接 */
esp_err_t network_service_set_wifi_config(const network_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    size_t ssid_len = strnlen(config->ssid, sizeof(config->ssid));
    size_t password_len = strnlen(config->password, sizeof(config->password));
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= NETWORK_WIFI_SSID_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid SSID length");
    ESP_RETURN_ON_FALSE(password_len == 0 ||
                        (password_len >= 8 && password_len <= NETWORK_WIFI_PASSWORD_MAX_LEN),
                        ESP_ERR_INVALID_ARG, TAG, "Wi-Fi password must be empty or 8-64 characters");
    ESP_RETURN_ON_ERROR(save_wifi_config(config), TAG, "Wi-Fi settings save failed");

    xSemaphoreTake(s_network_lock, portMAX_DELAY);
    s_wifi_config = *config;
    s_wifi_configured = true;
    s_saved_lease_valid = false;
    memset(&s_saved_lease, 0, sizeof(s_saved_lease));
    s_sta_connected = false;
    s_fallback_ap_active = false;
    s_connect_retries = 0;
    strlcpy(s_current_ip, "DHCP pending", sizeof(s_current_ip));
    xSemaphoreGive(s_network_lock);

    (void)esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "switch to STA mode failed");
    ESP_RETURN_ON_ERROR(configure_sta(config), TAG, "new Wi-Fi settings apply failed");
    esp_err_t ret = esp_wifi_connect();
    ESP_RETURN_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_WIFI_CONN, ret, TAG,
                        "router connection start failed");
    ESP_LOGI(TAG, "saved router settings: SSID=%s; requesting DHCP lease device=%s",
             config->ssid, s_device_id);
    return ESP_OK;
}