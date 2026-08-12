/**
 * storage.c — NVS 非易失存储模块实现
 *
 * 使用 ESP-IDF NVS API:
 *   - nvs_flash_init: 初始化 Flash
 *   - nvs_open / nvs_get_u8 / nvs_set_u8 / nvs_commit: 读写键值
 *
 * 存储键: 命名空间 "lamp_ctrl"
 *   bright_upper (uint8_t) — 上灯亮度
 *   bright_lower (uint8_t) — 下灯亮度
 *   sw_upper     (uint8_t) — 上灯开关 (0/1)
 *   sw_lower     (uint8_t) — 下灯开关 (0/1)
 *
 * 写入策略: 仅在调光松开/双击/关灯时写, 避免频繁写磨损 Flash
 */
#include "storage.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "STORAGE";

/* NVS 命名空间 */
#define NVS_NAMESPACE  "lamp_ctrl"

/* NVS 键名 */
#define KEY_BRIGHT_UPPER  "bright_u"
#define KEY_BRIGHT_LOWER  "bright_l"
#define KEY_SW_UPPER      "sw_u"
#define KEY_SW_LOWER      "sw_l"

/* ── 打开 NVS 句柄 (只读或读写) ── */
static nvs_handle_t open_nvs(nvs_open_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return 0;
    }
    return handle;
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void storage_init(void)
{
    /* 初始化 NVS Flash */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区已满或版本不兼容 → 擦除后重新初始化 */
        ESP_LOGW(TAG, "NVS erase and reinit: %s", esp_err_to_name(err));
        nvs_flash_erase();
        nvs_flash_init();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "NVS initialized, namespace='%s'", NVS_NAMESPACE);
}

void storage_load(lamp_status_t *status)
{
    nvs_handle_t h = open_nvs(NVS_READONLY);
    if (!h) {
        /* 打开失败 → 用默认值 */
        status->bright_upper = DEFAULT_BRIGHTNESS;
        status->bright_lower = DEFAULT_BRIGHTNESS;
        status->switch_upper = false;
        status->switch_lower = false;
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        return;
    }

    /* 读取亮度, 如果 key 不存在 (首次使用) 则用默认值 100 */
    int32_t val;
    esp_err_t err;

    err = nvs_get_i32(h, KEY_BRIGHT_UPPER, &val);
    status->bright_upper = (err == ESP_OK) ? (uint8_t)val : DEFAULT_BRIGHTNESS;

    err = nvs_get_i32(h, KEY_BRIGHT_LOWER, &val);
    status->bright_lower = (err == ESP_OK) ? (uint8_t)val : DEFAULT_BRIGHTNESS;

    err = nvs_get_i32(h, KEY_SW_UPPER, &val);
    status->switch_upper = (err == ESP_OK) ? (val != 0) : false;

    err = nvs_get_i32(h, KEY_SW_LOWER, &val);
    status->switch_lower = (err == ESP_OK) ? (val != 0) : false;

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: upper=%d%% %s | lower=%d%% %s",
             status->bright_upper, status->switch_upper ? "ON" : "OFF",
             status->bright_lower, status->switch_lower ? "ON" : "OFF");
}

void storage_save_brightness(lamp_id_t lamp, uint8_t brightness)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;

    /* 根据 lamp 选择对应的 key */
    const char *key = (lamp == LAMP_UPPER) ? KEY_BRIGHT_UPPER : KEY_BRIGHT_LOWER;
    nvs_set_i32(h, key, brightness);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGD(TAG, "saved brightness: lamp=%d val=%d", lamp, brightness);
}

void storage_save_switch_state(bool sw_upper, bool sw_lower)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;

    nvs_set_i32(h, KEY_SW_UPPER, sw_upper ? 1 : 0);
    nvs_set_i32(h, KEY_SW_LOWER, sw_lower ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGD(TAG, "saved switch: upper=%d lower=%d", sw_upper, sw_lower);
}
