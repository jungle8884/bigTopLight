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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "MAIN";

/* ═══════════════════════════════════════════════════
 * WiFi 连接成功回调 — 启动 MQTT
 * ═══════════════════════════════════════════════════ */
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected, starting MQTT client...");
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
                 running->label, FB_FIRMWARE_VERSION);
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
