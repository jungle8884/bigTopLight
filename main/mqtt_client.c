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
#include "state_machine.h"
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

/* ── LWT 遗嘱消息: {"status":4} (4=离线) ── */
static const char *s_lwt_msg = "{\"status\":4}";

/* ── ClientId: S&{deviceNum}&{productId}&{userId} ── */
static char s_client_id[64];

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
    cJSON_AddStringToObject(root, "firmwareVersion", FB_FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "status", 3);  /* 3=在线 */
    cJSON_AddStringToObject(root, "userId", FB_USER_ID);
    cJSON_AddNumberToObject(root, "longitude", 0);
    cJSON_AddNumberToObject(root, "latitude", 0);

    cJSON *summary = cJSON_CreateObject();
    cJSON_AddStringToObject(summary, "name", "XS-XLD5");
    cJSON_AddStringToObject(summary, "chip", "ESP32-C3");
    cJSON_AddStringToObject(summary, "version", FB_FIRMWARE_VERSION);
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
    /* 拼接 FastBee 主题前缀: /{productId}/{deviceNum} */
    snprintf(s_topic_prefix, sizeof(s_topic_prefix),
             "/%s/%s", FB_PRODUCT_ID, FB_DEVICE_NUM);

    /* 拼接完整主题字符串 */
    snprintf(s_topic_property_post, sizeof(s_topic_property_post),
             "%s/property/post", s_topic_prefix);
    snprintf(s_topic_function_get, sizeof(s_topic_function_get),
             "%s/function/get", s_topic_prefix);
    snprintf(s_topic_function_post, sizeof(s_topic_function_post),
             "%s/function/post", s_topic_prefix);
    snprintf(s_topic_info_post, sizeof(s_topic_info_post),
             "%s/info/post", s_topic_prefix);

    /* 拼接 ClientId: S&{deviceNum}&{productId}&{userId} */
    snprintf(s_client_id, sizeof(s_client_id),
             "S&%s&%s&%s", FB_DEVICE_NUM, FB_PRODUCT_ID, FB_USER_ID);

    /* 拼接 broker URI */
    snprintf(s_broker_uri, sizeof(s_broker_uri),
             "mqtt://%s:%d", FB_MQTT_HOST, FB_MQTT_PORT);

    ESP_LOGI(TAG, "FastBee config: broker=%s clientId=%s device=%s",
             s_broker_uri, s_client_id, FB_DEVICE_NUM);

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
