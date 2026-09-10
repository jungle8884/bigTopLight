/**
 * ota_app.c — OTA 固件升级实现
 *
 * 使用 ESP-IDF esp_https_ota 组件下载固件并写入 OTA 分区
 * 升级成功后自动切换启动分区并重启
 */
#include "ota_app.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mqtt_app.h"
#include "lamp_types.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "OTA";

/* 进度上报间隔 (每 10% 上报一次) */
#define OTA_REPORT_STEP 10

/* OTA 互斥锁: 防止并发 OTA 损坏正在写入的固件
 *  true = OTA 进行中 (拒绝重复触发); false = 空闲 */
static volatile bool s_ota_in_progress = false;

const char *ota_app_get_version(void)
{
    return FB_FIRMWARE_VERSION;
}

bool ota_app_need_upgrade(const char *target_version)
{
    if (!target_version || target_version[0] == '\0') {
        return true;  /* 无版本号, 默认升级 */
    }
    return strcmp(target_version, FB_FIRMWARE_VERSION) != 0;
}

/* 上报 OTA 进度到平台 */
static void report_ota_progress(const char *task_id, int progress, int status, const char *version)
{
    cJSON *root = cJSON_CreateObject();
    if (task_id && task_id[0]) {
        cJSON_AddStringToObject(root, "taskId", task_id);
    }
    cJSON_AddNumberToObject(root, "progress", progress);
    if (version && version[0]) {
        cJSON_AddStringToObject(root, "version", version);
    }
    cJSON_AddNumberToObject(root, "status", status);

    char *payload = cJSON_PrintUnformatted(root);
    mqtt_publish_ota_reply(payload);

    free(payload);
    cJSON_Delete(root);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

esp_err_t ota_app_start(const char *url, const char *task_id, const char *version)
{
    ESP_LOGI(TAG, "=== OTA Start ===");
    ESP_LOGI(TAG, "URL: %s", url);
    ESP_LOGI(TAG, "Version: %s", version ? version : "(null)");
    ESP_LOGI(TAG, "Current version: %s", FB_FIRMWARE_VERSION);

    /* 互斥检查: 防止并发 OTA 损坏两个分区 */
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring duplicate request");
        report_ota_progress(task_id, 0, 4, version);  /* status=4 失败 */
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_in_progress = true;

    /* FastBee 下发的是相对路径 (/profile/iot/...), 需拼接完整 URL */
    char full_url[OTA_URL_MAX_LEN + 64];
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        /* 已是完整 URL, 直接使用 */
        strncpy(full_url, url, sizeof(full_url) - 1);
        full_url[sizeof(full_url) - 1] = '\0';
    } else {
        /* 相对路径, 拼接基础地址 */
        snprintf(full_url, sizeof(full_url), "%s%s", FB_OTA_BASE_URL, url);
        ESP_LOGI(TAG, "Full URL: %s", full_url);
    }

    /* 上报: 开始升级 (status=2) */
    report_ota_progress(task_id, 0, 2, version);

    esp_http_client_config_t config = {
        .url = full_url,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,
        .timeout_ms = 60000,         /* 1MB+ 固件在弱网下需要更长超时 */
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .partial_http_download = true,
        .max_http_request_size = 2048,  /* ESP32-C3 RAM 紧张, 4KB 偏大 */
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        s_ota_in_progress = false;  /* 释放互斥锁 */
        report_ota_progress(task_id, 0, 4, version);  /* status=4 失败 */
        return ret;
    }

    int last_reported = -1;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int total = esp_https_ota_get_image_size(ota_handle);
        int read = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            int percent = (read * 100) / total;
            if (percent >= last_reported + OTA_REPORT_STEP) {
                last_reported = percent;
                ESP_LOGI(TAG, "Progress: %d%% (%d/%d bytes)", percent, read, total);
                report_ota_progress(task_id, percent, 2, version);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        s_ota_in_progress = false;  /* 释放互斥锁 */
        report_ota_progress(task_id, last_reported, 4, version);
        return ret;
    }

    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        s_ota_in_progress = false;  /* 释放互斥锁 */
        report_ota_progress(task_id, last_reported, 4, version);
        return ret;
    }

    ESP_LOGI(TAG, "OTA download complete, verifying...");

    /* esp_https_ota_finish() 内部已完成两件事:
     *   1. 校验固件签名/校验和
     *   2. 写入 otadata, 把刚烧录的分区设为 boot 分区
     * 因此此处严禁再调用 esp_ota_set_boot_partition() —
     * esp_ota_get_next_update_partition(NULL) 返回的是"对端"分区(下次 OTA 的写入目标),
     * 再次 set 会切到一个未烧录/旧固件的分区, 导致升级后启动失败
     */
    ESP_LOGI(TAG, "OTA success! Boot partition switched by finish()");

    /* 上报: 升级成功 (progress=100, status=3) */
    report_ota_progress(task_id, 100, 3, version);

    /* 等待 MQTT 消息发出 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Restarting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* 不会到达 */
}
