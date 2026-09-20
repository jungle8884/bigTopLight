/**
 * main.c — 应用入口
 *
 * 启动流程:
 *   1. NVS 初始化 (Wi-Fi + 存储共用)
 *   2. 状态机初始化 (加载记忆亮度, PWM, 指示灯上电序列)
 *   3. 按键初始化 + 启动按键检测任务
 *   4. 状态机任务启动
 *   5. Wi-Fi 连接管理启动 (SmartConfig 配网 + NVS 凭据)
 *   6. Wi-Fi 联网成功后启动 MQTT 客户端
 *
 * WiFi 配网说明:
 *   - 首次开机: NVS 无凭据 → 自动进入 SmartConfig
 *   - 用手机 ESP-TOUCH / Espressif Esptouch app 发送 WiFi 凭据
 *   - 设备收到后存入 NVS，掉电不丢失
 *   - 后续开机: 直接从 NVS 读取凭据连接
 *   - 连接失败超 5 次 → 自动回退 SmartConfig
 */
#include "lamp_types.h"
#include "storage.h"
#include "state_machine.h"
#include "button.h"
#include "pwm_drv.h"
#include "mqtt_app.h"
#include "wifi_app.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "MAIN";

/* ═══════════════════════════════════════════════════
 * SNTP 时间同步 — HTTPS OTA 的前提
 *
 * 设备上电后系统时间从 1970 起算, mbedTLS 校验服务器证书有效期时
 * 必然报 MBEDTLS_ERR_X509_CERT_VERIFY_FAILED("证书尚未生效")。
 * 所以只要走 HTTPS 下载固件, 联网后必须先校时。
 * 这里只做异步启动, 实际等待由 ota_app_start() 里的
 * wait_for_time_sync() 负责, 不会阻塞 MQTT 启动。
 * ═══════════════════════════════════════════════════ */
static bool s_sntp_started = false;

static void sntp_start(void)
{
    if (s_sntp_started) {
        return;   /* WiFi 断线重连会重复触发回调, 只初始化一次 */
    }
    s_sntp_started = true;

    ESP_LOGI(TAG, "Starting SNTP (required for HTTPS cert validation)");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    /* 注意: CONFIG_LWIP_SNTP_MAX_SERVERS=1, 只能配一个, 别加第二个 */
    esp_sntp_setservername(0, "ntp.aliyun.com");    /* 国内可达性最好 */
    esp_sntp_init();

    setenv("TZ", "CST-8", 1);   /* 东八区, 只影响日志时间显示 */
    tzset();
}

/* ═══════════════════════════════════════════════════
 * WiFi 连接成功回调 — 校时 + 启动 MQTT
 * ═══════════════════════════════════════════════════ */
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected");
    sntp_start();
    ESP_LOGI(TAG, "starting MQTT client...");
    mqtt_app_start();
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

    /* ── 0. OTA 启动验证 ──
     * 启用 rollback 后, 新固件首次启动处于 pending_verify 状态
     * 必须主动标记为 valid, 否则下次重启会回退到旧固件 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG, "OTA: first boot of new firmware, marking valid");
                esp_ota_mark_app_valid_cancel_rollback();
            }
        }
        ESP_LOGI(TAG, "Running partition: %s, firmware v%s",
                 running->label, esp_app_get_description()->version);
    }

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

    /* ── 6. Wi-Fi 连接管理 (SmartConfig + NVS 凭据) ──
     *    wifi_app_start 内部会:
     *    - 检查 NVS 是否有已保存的 WiFi 凭据
     *    - 有 → 直接连接; 无 → 进入 SmartConfig
     *    - 连接成功后回调 on_wifi_connected 启动 MQTT
     */
    wifi_app_set_connected_callback(on_wifi_connected);
    wifi_app_start();

    ESP_LOGI(TAG, "all modules initialized, system running");

    /* app_main 返回后, FreeRTOS 调度器继续运行各任务 */
}
