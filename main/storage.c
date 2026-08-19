/**
 * storage.c — NVS 非易失存储模块实现
 *
 * 存储键: 命名空间 "lamp_ctrl"
 *   灯状态: bright_u, bright_l, sw_u, sw_l
 *   WiFi 凭据: wifi_ssid, wifi_pwd
 *   设备信息: dev_user, dev_num, dev_auth
 */
#include "storage.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "STORAGE";

/* NVS 命名空间 */
#define NVS_NAMESPACE  "lamp_ctrl"

/* 灯控制键名 */
#define KEY_BRIGHT_UPPER  "bright_u"
#define KEY_BRIGHT_LOWER  "bright_l"
#define KEY_SW_UPPER      "sw_u"
#define KEY_SW_LOWER      "sw_l"

/* WiFi 凭据键名 */
#define KEY_WIFI_SSID     "wifi_ssid"
#define KEY_WIFI_PWD      "wifi_pwd"

/* 设备信息键名 (AP 配网下发) */
#define KEY_DEV_USER      "dev_user"
#define KEY_DEV_NUM       "dev_num"
#define KEY_DEV_AUTH      "dev_auth"

/* ── 打开 NVS 句柄 ── */
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
 * NVS 初始化 + 灯状态存取
 * ═══════════════════════════════════════════════════ */

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
        status->bright_upper = DEFAULT_BRIGHTNESS;
        status->bright_lower = DEFAULT_BRIGHTNESS;
        status->switch_upper = false;
        status->switch_lower = false;
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        return;
    }

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
    const char *key = (lamp == LAMP_UPPER) ? KEY_BRIGHT_UPPER : KEY_BRIGHT_LOWER;
    nvs_set_i32(h, key, brightness);
    nvs_commit(h);
    nvs_close(h);
}

void storage_save_switch_state(bool sw_upper, bool sw_lower)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;
    nvs_set_i32(h, KEY_SW_UPPER, sw_upper ? 1 : 0);
    nvs_set_i32(h, KEY_SW_LOWER, sw_lower ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ═══════════════════════════════════════════════════
 * WiFi 凭据存取
 * ═══════════════════════════════════════════════════ */

bool storage_load_wifi_creds(char *ssid, char *password,
                             size_t ssid_len, size_t pwd_len)
{
    nvs_handle_t h = open_nvs(NVS_READONLY);
    if (!h) return false;

    /* 读取 SSID (blob) */
    size_t required = ssid_len;
    esp_err_t err = nvs_get_blob(h, KEY_WIFI_SSID, ssid, &required);

    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "no saved WiFi credentials");
        return false;
    }

    /* 读取密码 (blob) */
    required = pwd_len;
    err = nvs_get_blob(h, KEY_WIFI_PWD, password, &required);

    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGW(TAG, "WiFi SSID found but password missing");
        return false;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded WiFi: SSID=%s", ssid);
    return true;
}

void storage_save_wifi_creds(const char *ssid, const char *password)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;

    nvs_set_blob(h, KEY_WIFI_SSID, ssid, strlen(ssid) + 1);
    nvs_set_blob(h, KEY_WIFI_PWD, password, strlen(password) + 1);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "saved WiFi: SSID=%s", ssid);
}

void storage_clear_wifi(void)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;
    nvs_erase_key(h, KEY_WIFI_SSID);
    nvs_erase_key(h, KEY_WIFI_PWD);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "cleared WiFi credentials");
}

/* ═══════════════════════════════════════════════════
 * 设备信息存取 (AP 配网下发)
 * ═══════════════════════════════════════════════════ */

bool storage_load_device_info(char *user_id, char *device_num,
                              char *auth_code)
{
    nvs_handle_t h = open_nvs(NVS_READONLY);
    if (!h) return false;

    size_t required = 16;
    esp_err_t err = nvs_get_blob(h, KEY_DEV_USER, user_id, &required);

    if (err != ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "no saved device info");
        return false;
    }

    required = 33;
    err = nvs_get_blob(h, KEY_DEV_NUM, device_num, &required);
    if (err != ESP_OK) {
        nvs_close(h);
        return false;
    }

    /* auth_code 可选，读不到置空 */
    required = 33;
    err = nvs_get_blob(h, KEY_DEV_AUTH, auth_code, &required);
    if (err != ESP_OK) {
        auth_code[0] = '\0';
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded device info: userId=%s deviceNum=%s",
             user_id, device_num);
    return true;
}

void storage_save_device_info(const char *user_id,
                              const char *device_num,
                              const char *auth_code)
{
    nvs_handle_t h = open_nvs(NVS_READWRITE);
    if (!h) return;

    nvs_set_blob(h, KEY_DEV_USER, user_id, strlen(user_id) + 1);
    nvs_set_blob(h, KEY_DEV_NUM, device_num, strlen(device_num) + 1);
    if (auth_code && strlen(auth_code) > 0) {
        nvs_set_blob(h, KEY_DEV_AUTH, auth_code, strlen(auth_code) + 1);
    }
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "saved device info: userId=%s deviceNum=%s",
             user_id, device_num);
}
