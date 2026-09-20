/**
 * mqtt_client.c — FastBee 平台 MQTT 客户端模块实现
 *
 * 使用 ESP-IDF MQTT 客户端 (esp-mqtt) + cJSON 解析
 * 对接 FastBee 物联网平台 (简单认证模式)
 *
 * 数据流:
 *   FastBee 平台 → MQTT function/get → parse_function_command() → state_machine_send_event()
 *   state_machine → mqtt_publish_state() → MQTT property/post  (属性: 亮度)
 *                                      → MQTT function/post  (功能: 开关/模式)
 */
#include "mqtt_app.h"          /* 本模块接口 */
#include "storage.h"           /* NVS 读取 deviceNum/userId */
#include "state_machine.h"
#include "ota_app.h"
#include "esp_log.h"
#include "esp_event.h"         /* esp_event_base_t, ESP_EVENT_ANY_ID */
#include "esp_wifi.h"          /* esp_wifi_sta_get_ap_info() for RSSI */
#include "mqtt_client.h"       /* ESP-IDF MQTT client API (esp_mqtt_*) */
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>           /* atoi() */

static const char *TAG = "MQTT";

/* ── MQTT 客户端句柄 ── */
static esp_mqtt_client_handle_t s_client = NULL;

/* ── FastBee 主题字符串 (运行时拼接) ── */
static char s_topic_prefix[64];          /* /{productId}/{deviceNum} */
static char s_topic_property_post[96];   /* .../property/post  (属性上报) */
static char s_topic_function_get[96];    /* .../function/get   (功能指令接收) */
static char s_topic_function_post[96];   /* .../function/post  (功能状态回复) */
static char s_topic_info_post[96];       /* .../info/post      (设备信息) */
static char s_topic_upgrade_set[96];     /* /{deviceNum}/http/upgrade/set (OTA下发 v2.0) */
static char s_topic_upgrade_set_v1[96];  /* /{productId}/{deviceNum}/upgrade/set (OTA下发 v1.x) */
static char s_topic_upgrade_reply[96];   /* /{deviceNum}/http/upgrade/reply (OTA回复 v2.0) */
static char s_topic_upgrade_reply_v1[96]; /* /{productId}/{deviceNum}/upgrade/reply (OTA回复 v1.x) */
static char s_topic_ota_get[96];        /* /{productId}/{deviceNum}/ota/get (OTA下发 - Web端实际格式) */
static char s_topic_ota_post[96];       /* /{productId}/{deviceNum}/ota/post (OTA回复 - Web端实际格式) */

/* ── LWT 遗嘱消息: {"status":4} (4=离线) ── */
static const char *s_lwt_msg = "{\"status\":4}";

/* ── ClientId: S&{deviceNum}&{productId}&{userId} ── */
static char s_client_id[64];
static char s_device_num[33];         /* 从 NVS 读取的设备编号 */
static char s_user_id[16];            /* 从 NVS 读取的用户 ID */

/* ── MQTT broker URI ── */
static char s_broker_uri[64];

/* ═══════════════════════════════════════════════════
 * 设备信息上报 (info/post)
 * ═══════════════════════════════════════════════════ */

static void publish_device_info(void)
{
    wifi_ap_record_t ap;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "rssi", rssi); 
    cJSON_AddStringToObject(root, "firmwareVersion", ota_app_get_version());
    cJSON_AddNumberToObject(root, "status", 3);  /* 3=在线 */
    cJSON_AddStringToObject(root, "userId", s_user_id);
    cJSON_AddNumberToObject(root, "longitude", 0);
    cJSON_AddNumberToObject(root, "latitude", 0);

    cJSON *summary = cJSON_CreateObject();
    cJSON_AddStringToObject(summary, "name", "XS-XLD5");
    cJSON_AddStringToObject(summary, "chip", "ESP32-C3");
    cJSON_AddStringToObject(summary, "version", ota_app_get_version());
    cJSON_AddItemToObject(root, "summary", summary);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        // 发布到 /{productId}/{deviceNum}/info/post
        esp_mqtt_client_publish(s_client, s_topic_info_post, 
                                json_str, 0, 1, 1);
        ESP_LOGD(TAG, "info post: %s", json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════
 * FastBee 功能指令解析 (JSON 数组格式)
 *
 * 输入格式: [{"id":"power","value":"1"},
 *            {"id":"brightness_upper","value":"80"}]
 * ═══════════════════════════════════════════════════ */

static void parse_function_command(const char *data, int len)
{
    /* 安全拷贝, 确保 null 结尾 */
    char json_str[256];
    int copy_len = len < 255 ? len : 255;
    memcpy(json_str, data, copy_len);
    json_str[copy_len] = '\0';
    /*MQTT event->data 不以 \0 结尾，不能直接传给 cJSON_Parse。
    这里拷贝到栈上 256 字节的缓冲区，手动添加 \0。
    超过 255 字节的数据会被截断，防止栈溢出。*/

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "function parse failed: %s", json_str);
        if (root) cJSON_Delete(root);
        return;
    }

    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        cJSON *id  = cJSON_GetObjectItem(item, "id");
        cJSON *val = cJSON_GetObjectItem(item, "value");
        if (!id || !id->valuestring || !val) continue;

        const char *vstr = val->valuestring ? val->valuestring : "";
        ESP_LOGI(TAG, "function: id=%s value=%s", id->valuestring, vstr);

        /**
          id	        对应宏	                构造的命令	                     含义
        "power"	        FB_PROP_POWER	        CMD_POWER_TOGGLE	            总开关
        "switch_upper"	FB_PROP_SWITCH_UPPER	CMD_LAMP_TOGGLE + LAMP_UPPER	上灯开关
        "switch_lower"	FB_PROP_SWITCH_LOWER	CMD_LAMP_TOGGLE + LAMP_LOWER	下灯开关
        "bright_upper"	FB_PROP_BRIGHT_UPPER	CMD_SET_BRIGHTNESS + LAMP_UPPER	上灯亮度
        "bright_lower"	FB_PROP_BRIGHT_LOWER	CMD_SET_BRIGHTNESS + LAMP_LOWER	下灯亮度
        "mode"	        FB_PROP_MODE	        CMD_SET_MODE	                模式切换
        */
        if (strcmp(id->valuestring, FB_PROP_POWER) == 0) {
            /* 总开关: {"id":"power","value":"1"} */
            system_msg_t msg = {
                .type   = MSG_COMMAND,
                .cmd_id = CMD_POWER_TOGGLE,
                .value  = (strcmp(vstr, "1") == 0) ? 1 : 0,
            };
            state_machine_send_event(&msg);

        } else if (strcmp(id->valuestring, FB_PROP_SWITCH_UPPER) == 0) {
            /* 上灯开关: {"id":"switch_upper","value":"1"} */
            system_msg_t msg = {
                .type    = MSG_COMMAND,
                .cmd_id  = CMD_LAMP_TOGGLE,
                .lamp_id = LAMP_UPPER,
                .value   = (strcmp(vstr, "1") == 0) ? 1 : 0,
            };
            state_machine_send_event(&msg);

        } else if (strcmp(id->valuestring, FB_PROP_SWITCH_LOWER) == 0) {
            /* 下灯开关: {"id":"switch_lower","value":"1"} */
            system_msg_t msg = {
                .type    = MSG_COMMAND,
                .cmd_id  = CMD_LAMP_TOGGLE,
                .lamp_id = LAMP_LOWER,
                .value   = (strcmp(vstr, "1") == 0) ? 1 : 0,
            };
            state_machine_send_event(&msg);

        } else if (strcmp(id->valuestring, FB_PROP_BRIGHT_UPPER) == 0) {
            /* 上灯亮度: {"id":"bright_upper","value":"80"} */
            system_msg_t msg = {
                .type    = MSG_COMMAND,
                .cmd_id  = CMD_SET_BRIGHTNESS,
                .lamp_id = LAMP_UPPER,
                .value   = atoi(vstr),
            };
            state_machine_send_event(&msg);

        } else if (strcmp(id->valuestring, FB_PROP_BRIGHT_LOWER) == 0) {
            /* 下灯亮度: {"id":"bright_lower","value":"80"} */
            system_msg_t msg = {
                .type    = MSG_COMMAND,
                .cmd_id  = CMD_SET_BRIGHTNESS,
                .lamp_id = LAMP_LOWER,
                .value   = atoi(vstr),
            };
            state_machine_send_event(&msg);

        } else if (strcmp(id->valuestring, FB_PROP_MODE) == 0) {
            /* 模式切换: {"id":"mode","value":"reading"} */
            int mode = 0;  /* normal */
            if (strcmp(vstr, MODE_STR_READING) == 0)      mode = 1;
            else if (strcmp(vstr, MODE_STR_NIGHT) == 0)   mode = 2;
            system_msg_t msg = {
                .type   = MSG_COMMAND,
                .cmd_id = CMD_SET_MODE,
                .value  = mode,
            };
            state_machine_send_event(&msg);

        } else {
            ESP_LOGW(TAG, "unknown function id: %s", id->valuestring);
        }
    }

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════
 * OTA 升级指令解析
 *
 * FastBee 下发格式 (兼容两种):
 *   v2.x: {"otaUrl":"http://...","firmwareVersion":"1.2","messageId":"...","taskId":1}
 *   文档: {"taskId":26,"url":"/profile/iot/1/xxx.bin","version":"1.2","status":1}
 * ═══════════════════════════════════════════════════ */

static void parse_ota_command(const char *data, int len)
{
    char json_str[512];
    int copy_len = len < 511 ? len : 511;
    memcpy(json_str, data, copy_len);
    json_str[copy_len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "OTA JSON parse failed: %s", json_str);
        return;
    }

    /* 提取 URL (兼容 otaUrl, url, downLoadUrl, downloadUrl 四种字段名) */
    cJSON *url_obj = cJSON_GetObjectItem(root, "otaUrl");
    if (!url_obj) url_obj = cJSON_GetObjectItem(root, "downLoadUrl");
    if (!url_obj) url_obj = cJSON_GetObjectItem(root, "downloadUrl");
    if (!url_obj) url_obj = cJSON_GetObjectItem(root, "url");

    /* 提取版本号 (兼容 firmwareVersion 和 version) */
    cJSON *ver_obj = cJSON_GetObjectItem(root, "firmwareVersion");
    if (!ver_obj) ver_obj = cJSON_GetObjectItem(root, "version");

    /* 提取 taskId */
    cJSON *task_obj = cJSON_GetObjectItem(root, "taskId");

    if (!url_obj || !cJSON_IsString(url_obj)) {
        ESP_LOGE(TAG, "OTA: missing url field");
        cJSON_Delete(root);
        return;
    }

    const char *url = url_obj->valuestring;
    /* 版本号可能是字符串 ("1.2") 或数字 (1.2), 统一转为字符串 */
    char version_buf[OTA_VERSION_MAX_LEN] = {0};
    const char *version = version_buf;
    if (ver_obj) {
        if (cJSON_IsString(ver_obj)) {
            strncpy(version_buf, ver_obj->valuestring, sizeof(version_buf) - 1);
        } else if (cJSON_IsNumber(ver_obj)) {
            snprintf(version_buf, sizeof(version_buf), "%.1f", ver_obj->valuedouble);
        }
    }
    char task_id[16] = {0};
    if (task_obj) {
        if (cJSON_IsString(task_obj)) {
            strncpy(task_id, task_obj->valuestring, sizeof(task_id) - 1);
        } else if (cJSON_IsNumber(task_obj)) {
            snprintf(task_id, sizeof(task_id), "%d", task_obj->valueint);
        }
    }

    ESP_LOGI(TAG, "OTA request: url=%s, version=%s, taskId=%s", url, version, task_id);

    /* 检查是否需要升级 */
    if (!ota_app_need_upgrade(version)) {
        ESP_LOGI(TAG, "Already at version %s, no upgrade needed", version);
        /* 回复: 已是最新版本 */
        cJSON *reply = cJSON_CreateObject();
        if (task_id[0]) cJSON_AddStringToObject(reply, "taskId", task_id);
        cJSON_AddNumberToObject(reply, "progress", 100);
        if (version[0]) cJSON_AddStringToObject(reply, "version", version);
        cJSON_AddNumberToObject(reply, "status", 3);  /* 成功 */
        char *payload = cJSON_PrintUnformatted(reply);
        mqtt_publish_ota_reply(payload);
        free(payload);
        cJSON_Delete(reply);
        cJSON_Delete(root);
        return;
    }

    /* 发送 OTA 启动事件到状态机 */
    system_msg_t msg = {0};
    msg.type = MSG_OTA_START;
    strncpy(msg.ota_url, url, sizeof(msg.ota_url) - 1);
    strncpy(msg.ota_task_id, task_id, sizeof(msg.ota_task_id) - 1);
    strncpy(msg.ota_version, version, sizeof(msg.ota_version) - 1);
    state_machine_send_event(&msg);

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════
 * MQTT 事件处理
 这是 MQTT 事件处理回调函数，由 ESP-IDF 的 esp-mqtt 库在各类 MQTT 事件发生时自动调用。
 它是整个 MQTT 通信的核心调度器。
 * ═══════════════════════════════════════════════════ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to FastBee broker");
            /* 订阅功能指令主题 (平台下发命令) */
            esp_mqtt_client_subscribe(s_client, s_topic_function_get, 1);
            /* 订阅 OTA 升级主题 — 兼容三种格式 */
            esp_mqtt_client_subscribe(s_client, s_topic_upgrade_set, 1);
            esp_mqtt_client_subscribe(s_client, s_topic_upgrade_set_v1, 1);
            esp_mqtt_client_subscribe(s_client, s_topic_ota_get, 1);
            /* 发布设备信息 (status=3 在线) */
            publish_device_info();
            break;

        case MQTT_EVENT_DISCONNECTED:
            /*仅记录日志。esp-mqtt 库会自动重连，无需手动处理。*/
            ESP_LOGI(TAG, "disconnected from FastBee broker");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            /*服务器确认订阅成功，仅记录日志。*/
            ESP_LOGI(TAG, "subscribed to %.*s", event->topic_len, event->topic);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "received: topic=%.*s data=%.*s",
                    event->topic_len, event->topic,
                    event->data_len, event->data);
            /* 判断是否为功能指令主题 */
            if (event->topic_len > 0 &&
                strncmp(event->topic, s_topic_function_get, event->topic_len) == 0 &&
                s_topic_function_get[event->topic_len] == '\0') {
                /*匹配成功 → 调用 parse_function_command 解析 JSON 指令，转化为 system_msg_t 投递到状态机队列*/
                parse_function_command(event->data, event->data_len);

            } else if (event->topic_len > 0 &&
                       ((strncmp(event->topic, s_topic_upgrade_set, event->topic_len) == 0 &&
                         s_topic_upgrade_set[event->topic_len] == '\0') ||
                        (strncmp(event->topic, s_topic_upgrade_set_v1, event->topic_len) == 0 &&
                         s_topic_upgrade_set_v1[event->topic_len] == '\0') ||
                        (strncmp(event->topic, s_topic_ota_get, event->topic_len) == 0 &&
                         s_topic_ota_get[event->topic_len] == '\0'))) {
                /* OTA 升级指令 → 解析并启动升级 (兼容 v2.0 和 v1.x 主题) */
                ESP_LOGI(TAG, "OTA upgrade command received");
                parse_ota_command(event->data, event->data_len);

            } else {
                /*匹配失败 → 记录警告日志*/
                ESP_LOGW(TAG, "unhandled topic: %.*s", event->topic_len, event->topic);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error type=%d", event->error_handle->error_type);
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void mqtt_app_start(void)
{
    /* 从 NVS 读取设备信息 (AP 配网时存入) */
    char auth_code[33] = {0};
    if (!storage_load_device_info(s_user_id, s_device_num, auth_code)) {
        ESP_LOGE(TAG, "No device info in NVS, AP config not done?");
        return;
    }
    
    /* 拼接 FastBee 主题前缀: /{productId}/{deviceNum} */
    snprintf(s_topic_prefix, sizeof(s_topic_prefix),
             "/%s/%s", FB_PRODUCT_ID, s_device_num);

    /* 拼接完整主题字符串 */
    snprintf(s_topic_property_post, sizeof(s_topic_property_post),
             "%s/property/post", s_topic_prefix);
    snprintf(s_topic_function_get, sizeof(s_topic_function_get),
             "%s/function/get", s_topic_prefix);
    snprintf(s_topic_function_post, sizeof(s_topic_function_post),
             "%s/function/post", s_topic_prefix);
    snprintf(s_topic_info_post, sizeof(s_topic_info_post),
             "%s/info/post", s_topic_prefix);
    /* OTA 主题格式与功能/属性主题不同, 同时兼容 v2.0 和 v1.x 两种格式 */
    /* v2.0: /{deviceNum}/http/upgrade/set */
    snprintf(s_topic_upgrade_set, sizeof(s_topic_upgrade_set),
             "/%s/http/upgrade/set", s_device_num);
    snprintf(s_topic_upgrade_reply, sizeof(s_topic_upgrade_reply),
             "/%s/http/upgrade/reply", s_device_num);
    /* v1.x: /{productId}/{deviceNum}/upgrade/set */
    snprintf(s_topic_upgrade_set_v1, sizeof(s_topic_upgrade_set_v1),
             "%s/upgrade/set", s_topic_prefix);
    snprintf(s_topic_upgrade_reply_v1, sizeof(s_topic_upgrade_reply_v1),
             "%s/upgrade/reply", s_topic_prefix);
    /* Web 端实际使用的 OTA 主题格式: /{productId}/{deviceNum}/ota/get */
    snprintf(s_topic_ota_get, sizeof(s_topic_ota_get),
             "%s/ota/get", s_topic_prefix);
    snprintf(s_topic_ota_post, sizeof(s_topic_ota_post),
             "%s/ota/post", s_topic_prefix);

    /* 拼接 ClientId: S&{deviceNum}&{productId}&{userId} */
    snprintf(s_client_id, sizeof(s_client_id),
             "S&%s&%s&%s", s_device_num, FB_PRODUCT_ID, s_user_id);

    /* 拼接 broker URI */
    snprintf(s_broker_uri, sizeof(s_broker_uri),
             "mqtt://%s:%d", FB_MQTT_HOST, FB_MQTT_PORT);

    ESP_LOGI(TAG, "FastBee config: broker=%s clientId=%s device=%s",
             s_broker_uri, s_client_id, s_device_num);

    /* MQTT 客户端配置 (ESP-IDF v5.4 结构体) */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_broker_uri,

        /* FastBee 简单认证: ClientId + 用户名 + 密码 */
        .credentials = {
            .client_id = s_client_id,
            .username  = FB_MQTT_USERNAME,
            .authentication = {
                .password = FB_MQTT_PASSWORD,
            },
        },

        /* LWT 遗嘱: 断开时自动发布 {"status":4} 到 info/post */
        .session.last_will = {
            .topic   = s_topic_info_post,
            .msg     = s_lwt_msg,
            .qos     = 1,
            .retain  = 1,
        },

        /* 缓冲区大小 */
        .buffer.size      = 1024,
        .buffer.out_size  = 1024,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt_client_init failed");
        return;
    }

    /* 注册事件处理器 */
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);

    /* 启动客户端 (自动重连) */
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started for FastBee platform");
}

void mqtt_publish_state(const lamp_status_t *status, system_state_t state)
{
    if (!s_client) return;

    /* ── 属性上报 (property/post): 只有 bright_upper, bright_lower ──
     * 物模型中这两个定义为"属性"类型，平台会存储历史并展示图表 */
    cJSON *prop_arr = cJSON_CreateArray();

    /* 上灯亮度 (开关关时上报 0) */
    char buf[8];
    uint8_t actual_upper = status->switch_upper ? status->bright_upper : 0;
    cJSON *p_bu = cJSON_CreateObject();
    cJSON_AddStringToObject(p_bu, "id", FB_PROP_BRIGHT_UPPER);
    snprintf(buf, sizeof(buf), "%u", (unsigned)actual_upper);
    cJSON_AddStringToObject(p_bu, "value", buf);
    cJSON_AddStringToObject(p_bu, "remark", "");
    cJSON_AddItemToArray(prop_arr, p_bu);

    /* 下灯亮度 (开关关时上报 0) */
    uint8_t actual_lower = status->switch_lower ? status->bright_lower : 0;
    cJSON *p_bl = cJSON_CreateObject();
    cJSON_AddStringToObject(p_bl, "id", FB_PROP_BRIGHT_LOWER);
    snprintf(buf, sizeof(buf), "%u", (unsigned)actual_lower);
    cJSON_AddStringToObject(p_bl, "value", buf);
    cJSON_AddStringToObject(p_bl, "remark", "");
    cJSON_AddItemToArray(prop_arr, p_bl);

    char *prop_str = cJSON_PrintUnformatted(prop_arr);
    if (prop_str) {
        esp_mqtt_client_publish(s_client, s_topic_property_post,
                                prop_str, 0, 1, 0);
        ESP_LOGD(TAG, "property post: %s", prop_str);
        free(prop_str);
    }
    cJSON_Delete(prop_arr);

    /* ── 功能状态回复 (function/post): power, mode, switch_upper, switch_lower ──
     * 物模型中这四个定义为"功能"类型，通过 function/post 上报当前状态 */
    cJSON *func_arr = cJSON_CreateArray();

    /* 总开关状态 */
    cJSON *f_power = cJSON_CreateObject();
    cJSON_AddStringToObject(f_power, "id", FB_PROP_POWER);
    cJSON_AddStringToObject(f_power, "value", (state != STATE_OFF) ? "1" : "0");
    cJSON_AddStringToObject(f_power, "remark", "");
    cJSON_AddItemToArray(func_arr, f_power);

    /* 模式 */
    const char *mode_str = MODE_STR_OFF;
    switch (state) {
    case STATE_OFF:     mode_str = MODE_STR_OFF;     break;
    case STATE_NORMAL:  mode_str = MODE_STR_NORMAL;  break;
    case STATE_READING: mode_str = MODE_STR_READING; break;
    case STATE_NIGHT:   mode_str = MODE_STR_NIGHT;   break;
    }
    cJSON *f_mode = cJSON_CreateObject();
    cJSON_AddStringToObject(f_mode, "id", FB_PROP_MODE);
    cJSON_AddStringToObject(f_mode, "value", mode_str);
    cJSON_AddStringToObject(f_mode, "remark", "");
    cJSON_AddItemToArray(func_arr, f_mode);

    /* 上灯开关 */
    cJSON *f_su = cJSON_CreateObject();
    cJSON_AddStringToObject(f_su, "id", FB_PROP_SWITCH_UPPER);
    cJSON_AddStringToObject(f_su, "value", status->switch_upper ? "1" : "0");
    cJSON_AddStringToObject(f_su, "remark", "");
    cJSON_AddItemToArray(func_arr, f_su);

    /* 下灯开关 */
    cJSON *f_sl = cJSON_CreateObject();
    cJSON_AddStringToObject(f_sl, "id", FB_PROP_SWITCH_LOWER);
    cJSON_AddStringToObject(f_sl, "value", status->switch_lower ? "1" : "0");
    cJSON_AddStringToObject(f_sl, "remark", "");
    cJSON_AddItemToArray(func_arr, f_sl);

    char *func_str = cJSON_PrintUnformatted(func_arr);
    if (func_str) {
        esp_mqtt_client_publish(s_client, s_topic_function_post,
                                func_str, 0, 1, 0);
        ESP_LOGD(TAG, "function post: %s", func_str);
        free(func_str);
    }
    cJSON_Delete(func_arr);
}

void mqtt_publish_brightness(lamp_id_t lamp, uint8_t brightness)
{
     if (!s_client) return;
     if (lamp >= LAMP_COUNT) return;

     const char* prop_id = (lamp == LAMP_UPPER) 
                            ? FB_PROP_BRIGHT_UPPER 
                            : FB_PROP_BRIGHT_LOWER;
    
    cJSON* prop_arr = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", prop_id);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)brightness);
    cJSON_AddStringToObject(item, "value", buf);
    cJSON_AddStringToObject(item, "remark", "亮度值");
    cJSON_AddItemToArray(prop_arr, item);

    char* prop_str = cJSON_PrintUnformatted(prop_arr);
    if (prop_str) {
        esp_mqtt_client_publish(s_client, s_topic_property_post, prop_str, 0, 1, 0);
        ESP_LOGD(TAG, "property post: %s", prop_str);
        free(prop_str);
    }
    cJSON_Delete(prop_arr);
}

void mqtt_publish_ota_reply(const char *payload)
{
    if (!s_client || !payload) return;

    /* 发布到 v2.0 格式主题 */
    esp_mqtt_client_publish(s_client, s_topic_upgrade_reply,
                            payload, 0, 1, 0);
    /* 同时发布到 v1.x 格式主题 */
    esp_mqtt_client_publish(s_client, s_topic_upgrade_reply_v1,
                            payload, 0, 1, 0);
    /* 同时发布到 Web 端使用的 ota/post 主题 */
    esp_mqtt_client_publish(s_client, s_topic_ota_post,
                            payload, 0, 1, 0);
    ESP_LOGI(TAG, "OTA reply: %s", payload);
}
