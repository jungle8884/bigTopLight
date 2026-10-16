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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

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
static esp_timer_handle_t s_retry_timer = NULL;

/* ═══════════════════════════════════════════════════
 * 断开原因解码 — 排查连不上的第一现场
 *
 * 日志里光打 "retry 1/5" 完全没有信息量: 找不到 AP(201) 和
 * 密码错(15/4WAY_HANDSHAKE_TIMEOUT) 的现象在应用层看来一模一样。
 * 必须把 reason 落下来, 才能区分是信号/环境问题还是凭据问题。
 * ═══════════════════════════════════════════════════ */
static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    /* ── 认证 / 关联阶段失败 ── */
    case WIFI_REASON_AUTH_EXPIRE:              return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:               return "AUTH_LEAVE";
    case WIFI_REASON_NO_AP_FOUND:              return "NO_AP_FOUND(没扫到这个SSID)";
    case WIFI_REASON_AUTH_FAIL:                return "AUTH_FAIL(认证帧被拒)";
    case WIFI_REASON_ASSOC_FAIL:               return "ASSOC_FAIL(关联被拒)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:        return "HANDSHAKE_TIMEOUT(握手超时)";
    case WIFI_REASON_CONNECTION_FAIL:          return "CONNECTION_FAIL";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   return "4WAY_HS_TIMEOUT(密码错误)";
    case WIFI_REASON_INVALID_PMKID:            return "INVALID_PMKID";
    case WIFI_REASON_802_1X_AUTH_FAILED:       return "802_1X_AUTH_FAILED";
    case WIFI_REASON_BAD_CIPHER_OR_AKM:        return "BAD_CIPHER_OR_AKM";

    /* ── 加密套件不匹配 ── */
    case WIFI_REASON_GROUP_CIPHER_INVALID:     return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:  return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:    return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_AKMP_INVALID:             return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:    return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:       return "INVALID_RSN_IE_CAP";

    /* ── 被 AP 主动踢掉 ── */
    case WIFI_REASON_ASSOC_TOOMANY:            return "ASSOC_TOO_MANY(AP满员)";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:      return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:     return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:  return "BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION: return "NOT_AUTHORIZED_THIS_LOC";
    case WIFI_REASON_PEER_INITIATED:           return "PEER_INITIATED(对端主动断开)";
    case WIFI_REASON_AP_INITIATED:             return "AP_INITIATED(AP主动踢人)";
    case WIFI_REASON_BEACON_TIMEOUT:           return "BEACON_TIMEOUT(信号丢失)";

    /* ── 扫描阶段的过滤 ── */
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY(加密方式不兼容)";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD(被authmode阈值过滤)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:      return "NO_AP_FOUND_IN_RSSI_THRESHOLD(信号低于阈值)";

    /* ── 其他 ── */
    case WIFI_REASON_STA_LEAVING:              return "STA_LEAVING";
    case WIFI_REASON_MIC_FAILURE:              return "MIC_FAILURE";
    default:                                   return "UNKNOWN";
    }
}

/* ═══════════════════════════════════════════════════
 * 退避重连定时器
 *
 * 为什么不能直接在断开回调里调 esp_wifi_connect():
 *   1. 回调在 event task 上下文, 阻塞调用会拖慢整个事件循环
 *   2. esp_wifi_connect() 返回值不检查的话, 上一次连接还没退干净时
 *      会返回 ESP_ERR_WIFI_CONN, 重试计数白花 —— 日志里
 *      "E wifi:sta is connecting, return error" 就是这么来的
 *   3. 5 次重试挤在 7 秒内打完, AP 侧还没恢复就被放弃
 *
 * 改成 esp_timer 异步重连: 每次重连前真正检查上一轮是否已结束,
 * 并按 1s/2s/4s/8s 退避, 给 AP 留出恢复时间。
 * ═══════════════════════════════════════════════════ */
static void retry_timer_cb(void *arg)
{
    if (s_ap_mode) {
        return;   /* 已转入 AP 模式, 无需再重连 */
    }

    /* 关键: 先确认是否已经拿到 IP。上一轮连接还没退干净时,
     * esp_wifi_connect() 会返回 ESP_ERR_WIFI_CONN, 重试计数白花
     * —— 日志里 "wifi:sta is connecting, return error" 就是这么来的。
     * 判据用 IP 而非 STA 状态机: IDF 5.4 已移除 esp_wifi_get_sta_status。*/
    if (xEventGroupGetBits(s_wifi_event_group) & BIT_CONNECTED) {
        ESP_LOGI(TAG, "retry %d/%d skipped, already got IP",
                 s_retry_count, WIFI_MAX_RETRY);
        s_retry_count = 0;
        return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "retry %d/%d: esp_wifi_connect() issued",
                 s_retry_count, WIFI_MAX_RETRY);
    } else {
        /* 上一次连接还没退干净, 不算这次重试有效 ——
         * 但下一次的 DISCONNECTED 事件会重新推进计数, 不会卡死 */
        ESP_LOGW(TAG, "retry %d/%d: esp_wifi_connect() busy (%s), "
                      "not consuming quota",
                 s_retry_count, WIFI_MAX_RETRY, esp_err_to_name(err));
    }
}

/* 按退避表安排下一次重连 */
static void schedule_retry(void)
{
    static const uint32_t delays[] = {
        WIFI_RETRY_DELAY_1, WIFI_RETRY_DELAY_2,
        WIFI_RETRY_DELAY_3, WIFI_RETRY_DELAY_MAX,
    };

    int idx = s_retry_count - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)(sizeof(delays) / sizeof(delays[0]))) {
        idx = (int)(sizeof(delays) / sizeof(delays[0])) - 1;
    }

    if (s_retry_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = retry_timer_cb,
            .name     = "wifi_retry",
        };
        /* 注意参数顺序: (create_args, out_handle) */
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_retry_timer));
    }

    ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d in %"PRIu32"ms",
             s_retry_count, WIFI_MAX_RETRY, delays[idx]);
    esp_timer_start_once(s_retry_timer, delays[idx] * 1000ULL);
}

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
    /* 与 wifi_app_start() 保持一致: 不限加密方式, 兼容 WPA/WPA2/WPA3 */
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    /* WiFi 已在 AP 模式启动过, 不需要再 esp_wifi_start(),
     * 直接重连即可 */
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    s_retry_count = 0;
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
static void start_ap_mode(void);

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

    /* WiFi 驱动可能已经启动过 (从 STA 模式回退过来),
     * esp_wifi_start() 对已启动的驱动会报错并重复初始化 netif,
     * 导致 DHCP server 被启动两次。只在未启动时才调。 */
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur_mode);
    bool need_start = (cur_mode == WIFI_MODE_NULL);

    /* 有重连定时器挂着一并撤掉 */
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    s_retry_count = 0;

    if (need_start) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return;
        }
    }
    ESP_LOGI(TAG, "AP mode started: SSID=%s, IP=192.168.4.1 (start=%s)",
             ap_ssid, need_start ? "yes" : "already running");

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
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "esp_wifi_connect() on STA_START: %s",
                             esp_err_to_name(err));
                }
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            /* 必须取出 reason/rssi —— 没有这两项, "连不上" 和
             * "密码错" 在应用层日志里完全无法区分 */
            {
                wifi_event_sta_disconnected_t *ev =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "STA disconnected: reason=%d(%s) rssi=%d dBm ssid_len=%d",
                         ev ? ev->reason : -1,
                         ev ? disconnect_reason_str(ev->reason) : "N/A",
                         ev ? ev->rssi : 0,
                         ev ? ev->ssid_len : 0);

                if (ev) {
                    char bssid[18];
                    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                             ev->bssid[0], ev->bssid[1], ev->bssid[2],
                             ev->bssid[3], ev->bssid[4], ev->bssid[5]);
                    ESP_LOGW(TAG, "  AP bssid=%s", bssid);
                }
            }

            xEventGroupClearBits(s_wifi_event_group, BIT_CONNECTED);
            if (s_ap_mode) break;  /* AP 模式下忽略 STA 断开 */

            s_retry_count++;
            if (s_retry_count <= WIFI_MAX_RETRY) {
                /* 不在这里直接调 esp_wifi_connect():
                 * 上一轮连接可能还没退干净, 会返回 ESP_ERR_WIFI_CONN,
                 * 重试配额被白白消耗。改由 esp_timer 退避后异步重连。 */
                schedule_retry();
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

            /* 连上了就撤掉待触发的重连, 避免连上后又被打断 */
            if (s_retry_timer) {
                esp_timer_stop(s_retry_timer);
            }

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
        /* threshold 用最低档 OPEN = 接受任意加密方式。
         * 硬编码 WIFI_AUTH_WPA2_PSK 会把 WPA-only 的老 AP
         * 直接过滤掉(reason 211 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD),
         * 表现同样是"连不上", 极难定位。 */
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_start();
        /* 连接交给 WIFI_EVENT_STA_START 事件统一发起。
         * 这里再调一次会和事件里的 esp_wifi_connect() 撞车,
         * 日志报 "wifi:sta is connecting, return error"。 */
    } else {
        /* 无凭据 → AP 模式配网 */
        ESP_LOGI(TAG, "No saved WiFi, starting AP config mode");
        start_ap_mode();
    }
}

void wifi_app_start_apconfig(void)
{
    ESP_LOGI(TAG, "Manual AP config trigger");
    /* 撤掉待触发的重连, 否则会和新进的 AP 模式打架 */
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
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
