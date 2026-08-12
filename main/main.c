/**
 * main.c — 应用入口
 *
 * 启动流程:
 *   1. NVS 初始化 (Wi-Fi + 存储共用)
 *   2. 状态机初始化 (加载记忆亮度, PWM, 指示灯上电序列)
 *   3. 按键初始化 + 启动按键检测任务
 *   4. 状态机任务启动
 *   5. Wi-Fi 连接
 *   6. Wi-Fi 联网成功后启动 MQTT 客户端
 *
 * ★ 使用前修改 lamp_types.h 中的 WIFI_SSID / WIFI_PASSWORD
 *   以及 FastBee 平台参数: FB_MQTT_HOST / FB_PRODUCT_ID / FB_DEVICE_NUM 等
 */
#include "lamp_types.h"
#include "storage.h"
#include "state_machine.h"
#include "button.h"
#include "pwm_drv.h"
#include "mqtt_app.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "MAIN";

/* WiFi 重连计数器 */
static int s_retry_count = 0;
#define WIFI_MAX_RETRY 5

/* ═══════════════════════════════════════════════════
 * Wi-Fi 事件处理
 * ═══════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {

        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* Wi-Fi 启动 → 开始连接 */
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            /* 断开 → 重试, 超过最大次数后暂停 */
            s_retry_count++;
            if (s_retry_count <= WIFI_MAX_RETRY) {
                ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "WiFi max retries reached, giving up");
            }
            break;

        default:
            break;
        }

    } else if (event_base == IP_EVENT) {

        if (event_id == IP_EVENT_STA_GOT_IP) {
            /* 获取到 IP → 连接成功 */
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_count = 0;

            /* Wi-Fi 联网成功 → 启动 MQTT 客户端 */
            ESP_LOGI(TAG, "starting MQTT client...");
            mqtt_app_start();
        }
    }
}

/* ═══════════════════════════════════════════════════
 * Wi-Fi 初始化 (Station 模式)
 * ═══════════════════════════════════════════════════ */

static void wifi_init_sta(void)
{
    /* 1. 创建默认 netif (Station) */
    esp_netif_create_default_wifi_sta();

    /* 2. 初始化 Wi-Fi 配置 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 3. 注册事件处理器 */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip);

    /* 4. 配置 SSID / 密码 (在 lamp_types.h 中定义) */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* 确保字符串终止 (防止 SSID 过长) */
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* 5. 启动 Wi-Fi */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA init done, SSID=%s", WIFI_SSID);
}

/* ═══════════════════════════════════════════════════
 * 应用入口
 * ═══════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ XS-XLD5 V3.0 Lamp Controller       ║");
    ESP_LOGI(TAG, "║ ESP32-C3 | ESP-IDF + FreeRTOS       ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    /* ── 1. 初始化底层网络和事件循环 (Wi-Fi 依赖) ── */
    ESP_ERROR_CHECK(esp_netif_init());

    /* 创建默认事件循环 (如果尚未创建) */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  
        ESP_ERROR_CHECK(ret);
    }

    /* ── 2. NVS 初始化 (存储模块内部调用) ── */
    storage_init();

    /* ── 3. 状态机初始化 ──
     *    - 创建事件队列
     *    - 从 NVS 加载记忆亮度
     *    - 初始化 PWM (LEDC)
     *    - 初始化指示灯 GPIO
     *    - 执行上电指示灯全亮 1s 序列
     *    - 创建调光/夜灯定时器
     */
    state_machine_init();

    /* ── 4. 按键初始化 + 启动检测任务 ── */
    button_init();
    button_start_task();

    /* ── 5. 启动状态机任务 ── */
    state_machine_start_task();

    /* ── 6. Wi-Fi 连接 (联网后自动启动 MQTT) ── */
    wifi_init_sta();

    ESP_LOGI(TAG, "all modules initialized, system running");

    /* app_main 返回后, FreeRTOS 调度器继续运行各任务 */
}
