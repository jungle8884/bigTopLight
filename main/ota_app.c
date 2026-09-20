/**
 * ota_app.c — OTA 固件升级实现
 *
 * 使用 ESP-IDF esp_https_ota 组件下载固件并写入 OTA 分区
 * 升级成功后自动切换启动分区并重启
 */
#include "ota_app.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mqtt_app.h"
#include "lamp_types.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "OTA";

/* 进度上报间隔 (每 10% 上报一次) */
#define OTA_REPORT_STEP 10

/* OTA 互斥锁: 防止并发 OTA 损坏正在写入的固件
 *  true = OTA 进行中 (拒绝重复触发); false = 空闲 */
static volatile bool s_ota_in_progress = false;

/* 判断 URL 里的 host 是否是设备端无法解析/无意义的主机名
 * 自建 FastBee 会按"浏览器访问地址"生成 localhost / 127.0.0.1,
 * 设备端没有该主机名解析, 直接下载必失败, 需要用 FB_OTA_BASE_URL 兜底 */
static bool is_unusable_host(const char *host)
{
    static const char *bad[] = {"localhost", "127.0.0.1", "0.0.0.0"};
    size_t host_len = strcspn(host, "/");   /* host 部分, 可能带 :port */

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t n = strlen(bad[i]);
        if (host_len >= n && strncmp(host, bad[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* 等待系统时间同步 — HTTPS 的前提
 * 设备上电后 time() 从 1970 起算, mbedTLS 校验证书有效期必然失败
 * (MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)。SNTP 由 main.c 在联网后异步启动,
 * 这里只负责等到时间真正有效。 */
static bool wait_for_time_sync(int timeout_ms)
{
    const time_t valid_after = 1700000000;   /* 2023-11-14, 早于此说明还没同步 */
    int waited = 0;

    while (waited < timeout_ms) {
        time_t now = 0;
        time(&now);
        if (now > valid_after) {
            struct tm t;
            localtime_r(&now, &t);
            ESP_LOGI(TAG, "System time ok: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }

    ESP_LOGW(TAG, "Time not synced after %d ms, HTTPS cert check may fail", timeout_ms);
    return false;
}

const char *ota_app_get_version(void)
{
    /* 版本号取自镜像内置的 esp_app_desc (由顶层 CMakeLists 的 PROJECT_VER 决定),
     * 保证 "镜像元数据" 和 "上报平台的版本" 永远一致, 不存在两处宏漂移 */
    return esp_app_get_description()->version;
}

bool ota_app_need_upgrade(const char *target_version)
{
    if (!target_version || target_version[0] == '\0') {
        return true;  /* 无版本号, 默认升级 */
    }
    return strcmp(target_version, ota_app_get_version()) != 0;
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
    ESP_LOGI(TAG, "Current version: %s", ota_app_get_version());

    /* 互斥检查: 防止并发 OTA 损坏两个分区 */
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring duplicate request");
        report_ota_progress(task_id, 0, 4, version);  /* status=4 失败 */
        return ESP_ERR_INVALID_STATE; 
    }
    s_ota_in_progress = true;

    /* ── 组装下载地址 ──────────────────────────────────────────────
     * FastBee 下发的 downloadUrl 有三种形态:
     *   1) 完整 URL 且 host 可用 → 原样使用 (在线平台 www.fleetbee.top)
     *   2) 完整 URL 但 host 是 localhost / 127.0.0.1 / 0.0.0.0
     *      ← 自建平台按"浏览器访问地址"生成, 设备端解析不了
     *      → 只取 path, 用编译期的 FB_OTA_BASE_URL 重拼
     *   3) 相对路径 → FB_OTA_BASE_URL + path
     *
     * ⚠ 绝不能无条件重拼 host: 在线平台 context-path 是 prod-api,
     *   自建是 dev-api, 无脑重拼会拼出 /prod-api/prod-api/... 双前缀 → 404 */
    char full_url[OTA_URL_MAX_LEN + 64];
    const char *host = NULL;

    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    }

    if (host && !is_unusable_host(host)) {
        /* 形态 1: 完整 URL 且 host 可用 → 原样下载 */
        snprintf(full_url, sizeof(full_url), "%s", url);
        ESP_LOGI(TAG, "Download URL: %s", full_url);
    } else {
        /* 形态 2/3: 用 FB_OTA_BASE_URL 重拼 */
        const char *path = host ? strchr(host, '/') : url;
        if (!path) {
            path = "";
        }

        /* FB_OTA_BASE_URL 末尾可能带 '/', 先削掉, 避免拼出 "//" */
        size_t base_len = strlen(FB_OTA_BASE_URL);
        while (base_len > 0 && FB_OTA_BASE_URL[base_len - 1] == '/') {
            base_len--;
        }

        if (path[0] == '/') {
            snprintf(full_url, sizeof(full_url), "%.*s%s", (int)base_len, FB_OTA_BASE_URL, path);
        } else {
            snprintf(full_url, sizeof(full_url), "%.*s/%s", (int)base_len, FB_OTA_BASE_URL, path);
        }
        ESP_LOGW(TAG, "URL host unusable, rebuilt: %s (platform gave: %s)", full_url, url);
    }

    /* HTTPS 必须校验证书有效期; 设备刚上电时系统时间是 1970, 握手必失败 */
    if (strncmp(full_url, "https://", 8) == 0) {
        wait_for_time_sync(15000);
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
        /* partial_http_download=false 是本项目的关键配置 (2026-09-20 实测踩坑):
         * 开启 partial 时, esp_https_ota_begin() 会先发 HTTP HEAD 请求,
         * 并从 HEAD 响应取 Content-Length 作为 image_length
         * (esp_https_ota.c:332-347)。fleetbee.top 对 HEAD 不返回
         * Content-Length → image_length 恒为 0 → perform() 永远进不了
         * SUCCESS 态 → finish() 静默跳过 set_boot_partition 却返回 ESP_OK
         * → otadata 未写 → 重启回旧固件且无任何报错。
         * 关掉后 image_length 改从 GET 响应取 (实测 1148640, 进度上报也正常),
         * 状态机能正常走到 SUCCESS。 */
        .partial_http_download = false,
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

    /* ── 手动兜底 set_boot_partition ──────────────────────────────
     * 实测出现过 finish() 返回 ESP_OK 但 otadata 没被写入的情况
     * (重启后 otadata[1]=0xffffffff, 设备跳回旧固件)。
     * esp_ota_get_next_update_partition(NULL) 基于 running 分区取对端:
     * running=ota_0 时返回 ota_1 = 刚烧录完的分区, 语义必然正确;
     * 若 finish 已正常切换, 这里重复写同一分区, 幂等无害 */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "FATAL: no update partition found after finish");
        s_ota_in_progress = false;
        report_ota_progress(task_id, 100, 4, version);
        return ESP_FAIL;
    }

    /* 只在 finish() 没切成功时才补刀: 已切对就不再重复做 image_validate + 写 otadata
     * (每次手动 set_boot 都会重新校验一遍镜像, 多花约 200ms) */
    const esp_partition_t *boot_now = esp_ota_get_boot_partition();
    if (boot_now && boot_now->address == update_part->address) {
        ESP_LOGI(TAG, "Boot partition already switched to %s by finish()", update_part->label);
    } else {
        ret = esp_ota_set_boot_partition(update_part);
        ESP_LOGI(TAG, "Manual esp_ota_set_boot_partition(%s): %s",
                 update_part->label, esp_err_to_name(ret));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "FATAL: cannot set boot partition, OTA aborted");
            s_ota_in_progress = false;
            report_ota_progress(task_id, 100, 4, version);
            return ret;
        }
    }

    /* 诊断: set_boot 之后立即确认 boot 指针确实切到了新分区。
     * 正常应打印 boot=ota_1; 若仍是 running 的对端没变, 说明 otadata 写入失败 */
    const esp_partition_t *boot_part = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "After set_boot: boot=%s running=%s",
             boot_part ? boot_part->label : "(null)",
             esp_ota_get_running_partition()->label);

    /* 上报: 升级成功 (progress=100, status=3) */
    report_ota_progress(task_id, 100, 3, version);

    /* 等待 MQTT 消息发出 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Restarting in 1 second...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* 不会到达 */
}
