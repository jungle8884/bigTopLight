/**
 * wifi_app.c — Wi-Fi 连接管理模块实现 (AP 模式配网)
 *
 * 兼容 FastBee App 配网流程:
 *   1. 设备进入 AP 模式，开启热点 FastBee-XXXX，IP 192.168.4.1
 *   2. HTTP Server 提供 /status (GET) 和 /config (POST) 接口
 *   3. App 检测设备后，下发 SSID + password + userId + deviceNum 等参数
 *   4. 设备保存参数到 NVS，切换 STA 模式连接 WiFi
 *   5. 连接成功后回调启动 MQTT
 *
 * 触发 AP 配网的条件:
 *   1. NVS 中无 WiFi 凭据 (首次使用)
 *   2. 直接连接重试超过 WIFI_MAX_RETRY 次仍失败
 *   3. 外部调用 wifi_app_start_apconfig() (如按键长按)
 */
#include "wifi_app.h"
#include "storage.h"
#include "lamp_types.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

/* ── 事件位 ── */
#define BIT_CONNECTED    BIT0   /* 已获取 IP */

/* ── 内部状态 ── */
static EventGroupHandle_t s_wifi_event_group = NULL;
static void (*s_connected_cb)(void) = NULL;
static int s_retry_count = 0;
static bool s_ap_mode = false;
static httpd_handle_t s_http_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

/* ═══════════════════════════════════════════════════
 * HTTP Server — /status 检测接口 (GET)
 * App 轮询此接口，返回 200 表示设备就绪
 * ═══════════════════════════════════════════════════ */
static esp_err_t status_handler(httpd_req_t *req)
{
    /* CORS 头 (兼容 H5/浏览器) */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "AP配网已准备就绪",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 * HTTP Server — /config 配网接口 (POST)
 * 接收参数: SSID, password, userId, deviceNum, authCode, extra
 * 参数通过 URL query string 传递 (FastBee App 约定)
 * ═══════════════════════════════════════════════════ */
static esp_err_t config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Received /config request");

    /* 提取 URL query string */
    char query[512] = {0};
    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get query string");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "配网参数解析失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Query: %s", query);

    /* 解析必填参数 */
    char ssid[33] = {0};
    char password[65] = {0};
    char user_id[16] = {0};

    httpd_query_key_value(query, "SSID", ssid, sizeof(ssid));
    httpd_query_key_value(query, "password", password, sizeof(password));
    httpd_query_key_value(query, "userId", user_id, sizeof(user_id));

    if (strlen(ssid) == 0 || strlen(password) == 0 || strlen(user_id) == 0) {
        ESP_LOGE(TAG, "Missing required params: SSID=%s userId=%s", ssid, user_id);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "配网必须传递用户编号、WIFI名称和WIFI密码，配网失败");
        return ESP_FAIL;
    }

    /* 解析可选参数 */
    char device_num[33] = {0};
    char auth_code[33] = {0};
    httpd_query_key_value(query, "deviceNum", device_num, sizeof(device_num));
    httpd_query_key_value(query, "authCode", auth_code, sizeof(auth_code));

    /* 如果未传 deviceNum，用 MAC 地址生成一个 */
    if (strlen(device_num) == 0) {
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(device_num, sizeof(device_num),
                 "D%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Generated deviceNum from MAC: %s", device_num);
    }

    ESP_LOGI(TAG, "Config: SSID=%s userId=%s deviceNum=%s",
             ssid, user_id, device_num);

    /* 保存所有参数到 NVS */
    storage_save_wifi_creds(ssid, password);
    storage_save_device_info(user_id, device_num,
                             strlen(auth_code) > 0 ? auth_code : "");

    /* 先发 HTTP 200 响应，再切换 WiFi 模式 */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "设备已更新WIFI配置，开始连接WIFI...",
                    HTTPD_RESP_USE_STRLEN);

    /* 延迟 2 秒确保响应送达手机，然后切换模式 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 停止 HTTP Server */
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    /* 切换到 STA 模式连接 WiFi */
    s_ap_mode = false;

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 * HTTP Server — OPTIONS 预检请求处理 (CORS)
 * ═══════════════════════════════════════════════════ */
static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 * 启动 AP 模式 + HTTP Server
 * ═══════════════════════════════════════════════════ */
static void start_ap_mode(void)
{
    if (s_ap_mode) return;
    s_ap_mode = true;
    s_retry_count = 0;

    /* 生成 AP 热点名: FastBee-XXXX (MAC 后 4 hex) */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid),
             "FastBee-%02X%02X", mac[4], mac[5]);

    /* 创建 AP netif (默认 IP 192.168.4.1) */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* 设置 AP+STA 模式 (配网后可直接切 STA) */
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    /* 配置 AP 热点 (开放式网络) */
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    esp_wifi_start();
    ESP_LOGI(TAG, "AP mode started: SSID=%s, IP=192.168.4.1", ap_ssid);

    /* 启动 HTTP Server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 4;

    if (httpd_start(&s_http_server, &http_cfg) == ESP_OK) {
        /* 注册 /status (GET) */
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
        };
        httpd_register_uri_handler(s_http_server, &status_uri);

        /* 注册 /config (POST) */
        httpd_uri_t config_uri = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = config_handler,
        };
        httpd_register_uri_handler(s_http_server, &config_uri);

        /* 注册 OPTIONS 处理 (CORS 预检) */
        httpd_uri_t options_uri = {
            .uri = "/*",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
        };
        httpd_register_uri_handler(s_http_server, &options_uri);

        ESP_LOGI(TAG, "HTTP Server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP Server");
    }
}

/* ═══════════════════════════════════════════════════
 * WiFi 事件处理
 * ═══════════════════════════════════════════════════ */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {

        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!s_ap_mode) {
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_wifi_event_group, BIT_CONNECTED);
            if (s_ap_mode) break;  /* AP 模式下忽略 STA 断开 */

            s_retry_count++;
            if (s_retry_count <= WIFI_MAX_RETRY) {
                ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "WiFi max retries, fallback to AP mode");
                start_ap_mode();
            }
            break;

        default:
            break;
        }

    } else if (event_base == IP_EVENT) {

        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR,
                     IP2STR(&event->ip_info.ip));
            s_retry_count = 0;
            xEventGroupSetBits(s_wifi_event_group, BIT_CONNECTED);

            if (s_connected_cb) {
                s_connected_cb();
            }
        }
    }
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void wifi_app_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* 创建 STA netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* 初始化 WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册事件处理器 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    /* 检查 NVS 是否有 WiFi 凭据 */
    char ssid[33] = {0};
    char password[65] = {0};
    bool has_credentials = storage_load_wifi_creds(ssid, password, sizeof(ssid), sizeof(password));

    if (has_credentials) {
        /* 有凭据 → STA 模式直接连接 */
        ESP_LOGI(TAG, "Found saved WiFi, connecting to SSID:%s", ssid);

        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_start();
        esp_wifi_connect();
    } else {
        /* 无凭据 → AP 模式配网 */
        ESP_LOGI(TAG, "No saved WiFi, starting AP config mode");
        start_ap_mode();
    }
}

void wifi_app_start_apconfig(void)
{
    ESP_LOGI(TAG, "Manual AP config trigger");
    /* 停止 HTTP Server (如果已运行) */
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    /* 断开当前连接 */
    esp_wifi_disconnect();
    /* 清除 NVS 中的旧凭据 */
    storage_clear_wifi();
    /* 进入 AP 模式 */
    start_ap_mode();
}

void wifi_app_set_connected_callback(void (*cb)(void))
{
    s_connected_cb = cb;
}
