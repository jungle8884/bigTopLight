# OTA 固件升级全链路分析与实施指南

> 涉及：后端 (SpringBoot) + 前端 (Vue Web管理端) + MCU (ESP32-C3)
> 目标：从 Web 端发起 OTA，实现设备远程固件升级

---

## 目录

- [1. 现状评估](#1-现状评估)
- [2. 后端 OTA 架构（已完整）](#2-后端-ota-架构已完整)
- [3. 前端 OTA 页面（已编译）](#3-前端-ota-页面已编译)
- [4. MCU 端 OTA（待实现）](#4-mcu-端-ota待实现)
- [5. MQTT 消息格式（关键）](#5-mqtt-消息格式关键)
- [6. 完整 OTA 流程](#6-完整-ota-流程)
- [7. 实施步骤](#7-实施步骤)
- [8. 调试与验证](#8-调试与验证)

---

## 1. 现状评估

| 层 | 状态 | 说明 |
|----|------|------|
| **后端** | 已完整 | 固件管理 + 升级任务 + Redis消息队列 + MQTT下发，全链路已实现 |
| **前端 (Vue)** | 已编译 | `dist/` 中有固件管理页面，可正常使用，但无源码无法修改 |
| **MCU (ESP32-C3)** | 未实现 | 只有版本号定义和上报，无OTA分区、无下载、无升级逻辑 |

**结论**：后端和前端已经就绪，主要工作量在 MCU 端实现 OTA。

---

## 2. 后端 OTA 架构（已完整）

### 2.1 数据库表

```
iot_firmware              ← 固件管理表（存储固件文件路径、版本号）
iot_firmware_task         ← 升级任务表（任务名称、固件ID、升级类型）
iot_firmware_task_detail  ← 升级任务详情表（每台设备的升级状态）
```

设备状态流转（`upgrade_status` 字段）：

```
0(等待升级) → 1(已发送) → 2(升级中) → 3(成功) / 4(失败) / 5(停止)
```

### 2.2 核心 API 端点

#### 固件管理 API（`/iot/firmware`）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/iot/firmware/list` | 固件列表 |
| GET | `/iot/firmware/{firmwareId}` | 固件详情 |
| GET | `/iot/firmware/getLatest/{deviceId}` | 设备最新固件 |
| POST | `/iot/firmware` | 新增固件（上传文件后填写路径） |
| PUT | `/iot/firmware` | 修改固件 |
| DELETE | `/iot/firmware/{firmwareIds}` | 删除固件 |

#### 升级任务 API（`/iot/firmware/task`）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/iot/firmware/task/list` | 任务列表 |
| GET | `/iot/firmware/task/{id}` | 任务详情 |
| POST | `/iot/firmware/task` | **创建任务并触发升级** |
| POST | `/iot/firmware/task/upgrade` | 手动触发升级 |
| GET | `/iot/firmware/task/upgrade/detail` | 升级详情列表 |
| GET | `/iot/firmware/task/deviceStatistic` | 升级统计 |
| GET | `/iot/firmware/task/deviceList` | 设备列表 |

### 2.3 后端处理链路

```
Web端点击"升级"
  → POST /iot/firmware/task (创建任务)
      → FirmwareTaskController.add()
          → firmwareTaskService.insertFirmwareTask()  创建任务+明细(status=0)
          → 判断是否有预定时间
              ├─ 有预定时间 → DelayUpgradeQueue.offerTask() 延迟执行
              └─ 无预定时间 → otaUpgradeService.upgrade() 立即执行
                  → 查询固件信息
                  → 遍历设备列表，对每个设备:
                      → 构建 OtaUpgradeBo 消息体
                      → 校验设备是否在线 (MqttRemoteManager.checkDeviceStatus)
                      → messagePublishService.publish(upgradeBo, "device_upgrade")
                          → Redis Channel 发布
                              → RedisChannelConsume.onMessage() 监听
                                  → OtaUpgradeQueue.offer() 入队列
                                      → UpgradeListen.listen() 异步消费
                                          → MqttMessagePublishImpl.upGradeOTA()
                                              → 构建完整 URL: http://服务器IP + filePath
                                              → 构建 MQTT topic: /{productId}/{serialNumber}/upgrade/set
                                              → protocol.encode() 编码消息
                                              → mqttClient.publish() 发送
                                              → firmwareTaskDetailService.update(bo, SEND) 状态改为1
```

### 2.4 关键源码位置

| 功能 | 文件路径 |
|------|----------|
| 固件Controller | `fastbee-open-api/.../controller/firmware/FirmwareController.java` |
| 任务Controller | `fastbee-open-api/.../controller/firmware/FirmwareTaskController.java` |
| OTA升级Service | `fastbee-open-api/.../service/impl/OtaUpgradeServiceImpl.java` |
| MQTT发布实现 | `fastbee-mqtt/.../service/impl/MqttMessagePublishImpl.java` (upGradeOTA方法) |
| OTA消息体 | `fastbee-common/.../mq/ota/OtaUpgradeBo.java` |
| 升级状态枚举 | `fastbee-common/.../enums/OTAUpgrade.java` |
| 主题类型枚举 | `fastbee-common/.../enums/TopicType.java` |
| 主题拼接工具 | `fastbee-common/.../utils/gateway/mq/TopicsUtils.java` |
| Redis消费监听 | `fastbee-mq/.../redischannel/consumer/RedisChannelConsume.java` |
| OTA队列消费 | `fastbee-mq/.../redischannel/listen/UpgradeListen.java` |
| 固件缓存 | `fastbee-iot-service/.../cache/impl/FirmwareCacheImpl.java` |

---

## 3. 前端 OTA 页面（已编译）

### 3.1 前端现状

```
vue/
├── dist/           ← 编译后的静态文件（可直接部署）
│   ├── index.html
│   └── static/js/
│       ├── app.xxx.js                    ← 路由+框架
│       └── chunk-854b258a.xxx.js         ← 固件管理页面（FirmwareTask组件）
└── 无 src/ 源码目录
```

**前端已有功能**（从编译后 JS 分析）：
- 固件信息展示（名称、产品、版本、时间、描述）
- 固件升级设备统计（总数、成功、升级中、失败）
- 任务明细 Tab（任务ID、名称、类型、设备数量、预定时间）
- 设备明细 Tab（设备序列号、名称、升级状态筛选）
- 升级状态：待推送、升级中、升级成功、升级失败、停止

### 3.2 使用方式

直接部署 `dist/` 到 Nginx 或使用现有的 Web 管理后台即可，固件管理页面路径为 `/iot/firmware`。

---

## 4. MCU 端 OTA（已实现）

### 4.1 实现状态

```
MCU (C:\Users\jungle\test)
  ├── lamp_types.h        → FB_FIRMWARE_VERSION "2.0", MSG_OTA_START, OTA 字段, FB_OTA_BASE_URL
  ├── mqtt_client.c       → 订阅 OTA 主题, 解析指令, 上报进度
  ├── CMakeLists.txt      → 已添加 esp_https_ota, esp_http_client, esp-tls 依赖
  ├── sdkconfig.defaults  → 自定义分区表, OTA 回滚, HTTP 允许, 证书包
  ├── partitions.csv      → factory + ota_0 + ota_1 三分区 (各 1MB)
  ├── ota_app.c/.h        → OTA 下载 + 写入 + 进度上报 + 重启
  ├── state_machine.c     → handle_ota_start, MSG_OTA_START 事件处理
  ├── indicator.c/.h      → indicator_set_all (升级时指示灯全亮)
  └── main.c              → OTA rollback 验证 (新固件首次启动标记 valid)
```

---

## 5. MQTT 消息格式（关键）

### 5.1 OTA 下发主题（平台 → 设备）

设备同时订阅两种格式，兼容 FastBee v2.0 和 v1.x：

```
v2.0: /{serialNumber}/http/upgrade/set
v1.x: /{productId}/{serialNumber}/upgrade/set
```

### 5.2 OTA 下发消息体

FastBee 下发的 JSON 消息（兼容两种字段名）：

**v2.0 文档格式:**
```json
{
    "taskId": 26,
    "url": "/profile/iot/1/2026-0909-140855.bin",
    "version": 1.2,
    "status": 1
}
```

**v1.x 后端格式:**
```json
{
    "otaUrl": "http://192.168.1.100/profile/iot/1/firmware.bin",
    "firmwareVersion": "1.1",
    "taskId": 1,
    "messageId": "1234567890"
}
```

MCU 代码兼容两种字段名:
- URL: `otaUrl` 或 `url`
- 版本: `firmwareVersion` 或 `version` (字符串或数字)
- taskId: 数字或字符串

**URL 处理:**
- 如果 URL 以 `http://` 或 `https://` 开头, 直接使用
- 如果是相对路径 (`/profile/iot/...`), 拼接 `FB_OTA_BASE_URL` ("https://www.fleetbee.top/prod-api")

### 5.3 OTA 回复主题（设备 → 平台）

设备同时发布到两种格式:

```
v2.0: /{serialNumber}/http/upgrade/reply
v1.x: /{productId}/{serialNumber}/upgrade/reply
```

### 5.4 OTA 回复消息体（设备 → 平台）

```json
{"taskId":"26","progress":0,"version":"1.2","status":2}   // 开始升级
{"taskId":"26","progress":50,"version":"1.2","status":2}  // 升级中
{"taskId":"26","progress":100,"version":"1.2","status":3} // 升级成功
{"taskId":"26","progress":50,"version":"1.2","status":4}  // 升级失败
```

| status | 含义 |
|--------|------|
| 0 | 等待升级 |
| 1 | 已发送 |
| 2 | 升级中 |
| 3 | 成功 |
| 4 | 失败 |

### 5.5 设备版本上报

设备在 `info/post` 上报中包含版本号：

```json
{
    "rssi": -55,
    "firmwareVersion": "2.0",
    "status": 3,
    ...
}
```

升级成功重启后，新固件上报新版本号，平台据此判断升级是否生效。

---

## 6. 完整 OTA 流程

```
┌─────────────┐     ┌──────────────┐     ┌───────────────┐     ┌───────────┐
│  Web管理端   │     │  SpringBoot  │     │  MQTT Broker  │     │  ESP32-C3 │
│  (Vue dist)  │     │   后端        │     │               │     │   设备     │
└──────┬───────┘     └──────┬───────┘     └───────┬───────┘     └─────┬─────┘
       │                     │                     │                   │
  1. 上传固件文件              │                     │                   │
       │ POST /iot/firmware  │                     │                   │
       ├────────────────────►│                     │                   │
       │                     │ 保存固件记录          │                   │
       │                     │ filePath=/profile/...│                   │
       │                     │                     │                   │
  2. 创建升级任务              │                     │                   │
       │ POST /iot/firmware/task                  │                   │
       ├────────────────────►│                     │                   │
       │                     │ 创建任务+明细(状态=0)  │                   │
       │                     │ 构建 OtaUpgradeBo    │                   │
       │                     │ otaUrl=http://IP+path│                   │
       │                     │ 发布到 Redis Channel │                   │
       │                     │                     │                   │
  3. Redis消费 → MQTT发布     │                     │                   │
       │                     │ publish 到 upgrade/set │                │
       │                     ├────────────────────►│                   │
       │                     │                     │ 转发给设备          │
       │                     │                     ├──────────────────►│
       │                     │                     │                   │
       │                     │                     │  4. 设备收到OTA指令 │
       │                     │                     │                   │
  5. 设备回复"收到"             │                     │                   │
       │                     │                     │  publish 到       │
       │                     │                     │  upgrade/reply    │
       │                     │                     │◄──────────────────┤
       │                     │ 更新状态=2(升级中)    │                   │
       │                     │                     │                   │
       │                     │                     │  6. HTTP下载固件   │
       │                     │                     │  esp_https_ota    │
       │                     │  ◄── HTTP GET 固件 ──────────────────────┤
       │                     │  ── 固件二进制流 ──────────────────────►│
       │                     │                     │                   │
       │                     │                     │  7. 写Flash + 校验 │
       │                     │                     │  8. 切换启动分区   │
       │                     │                     │  9. 重启           │
       │                     │                     │                   │
       │                     │                     │  10. 新固件启动     │
       │                     │                     │  上报 info/post   │
       │                     │                     │  version=1.1      │
       │                     │                     │◄──────────────────┤
       │                     │ 更新设备固件版本       │                   │
       │                     │ 更新状态=3(成功)      │                   │
       │                     │                     │                   │
  11. 查看升级结果             │                     │                   │
       │ GET /iot/firmware/task/upgrade/detail     │                   │
       ├────────────────────►│                     │                   │
       │◄────────────────────┤ 返回升级详情          │                   │
       │                     │                     │                   │
```

---

## 7. 实施步骤

### Step 1: 固件准备（Web端）

在 Web 管理端操作：

1. 进入产品管理 → 选择产品 → 确保产品已发布(status=2)
2. 进入固件管理页面 → 新增固件
   - 填写固件名称、版本号、关联产品
   - 固件文件路径：先通过文件上传功能上传 `.bin` 文件，获取路径
3. 确认固件记录创建成功

### Step 2: MCU 分区表配置

**新建文件**：`partitions.csv`

```csv
# Name,   Type, SubType,  Offset,  Size,    Flags
# NVS
nvs,       data, nvs,     0x9000,  0x6000,
# PHY Init
phy_init,  data, phy,     0xf000,  0x1000,
# Factory app
factory,   app,  factory, 0x10000, 1M,
# OTA 0
ota_0,     app,  ota_0,   ,        1M,
# OTA 1
ota_1,     app,  ota_1,   ,        1M,
```

> ESP32-C3 Flash 4MB，Factory + OTA0 + OTA1 各 1MB 可行。

### Step 3: MCU 依赖和配置

**修改 `CMakeLists.txt`**，添加 OTA 依赖：

```cmake
idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "pwm_drv.c"
        "storage.c"
        "indicator.c"
        "timer.c"
        "state_machine.c"
        "mqtt_client.c"
        "wifi_app.c"
        "ota_app.c"                    # 新增
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_timer
        esp_wifi
        esp_http_server
        mqtt
        driver
        json
        esp_https_ota                  # 新增
        esp_http_client               # 新增
)
```

**修改 `sdkconfig.defaults`**：

```ini
# 分区表 — 改为自定义分区表
# CONFIG_PARTITION_TABLE_SINGLE_APP=y   # 注释掉
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# 启用 OTA
CONFIG_APP_BUILD_TYPE_RAM=n
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# HTTP Client (用于下载固件)
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y

# 允许 HTTP 下载（如果固件服务器是 HTTP 而非 HTTPS）
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y
```

### Step 4: MCU OTA 模块实现

**新建文件**：`main/ota_app.h`

```c
#ifndef OTA_APP_H
#define OTA_APP_H

#include "esp_err.h"
#include "esp_https_ota.h"

/* OTA 状态回调 */
typedef void (*ota_progress_cb_t)(int percent, const char *msg);

/* 启动 OTA 升级
 * @param url 固件下载地址
 * @param cb  进度回调（可为 NULL）
 * @return ESP_OK 成功，其他失败
 */
esp_err_t ota_app_start(const char *url, ota_progress_cb_t cb);

/* 获取当前固件版本 */
const char *ota_app_get_version(void);

/* 检查是否需要升级 */
bool ota_app_need_upgrade(const char *target_version);

#endif
```

**新建文件**：`main/ota_app.c`（核心实现）

```c
#include "ota_app.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "lamp_types.h"
#include <string.h>

static const char *TAG = "OTA";

const char *ota_app_get_version(void)
{
    return FB_FIRMWARE_VERSION;
}

bool ota_app_need_upgrade(const char *target_version)
{
    if (!target_version) return false;
    /* 简单字符串比较，实际可用 atof 做数值比较 */
    return strcmp(target_version, FB_FIRMWARE_VERSION) != 0;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    /* HTTP 事件回调，可留空或用于日志 */
    return ESP_OK;
}

esp_err_t ota_app_start(const char *url, ota_progress_cb_t cb)
{
    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        if (cb) cb(0, "OTA begin failed");
        return ret;
    }

    /* 循环下载固件 */
    int last_percent = -1;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        /* 获取进度 */
        int total = esp_https_ota_get_image_size(ota_handle);
        int read = esp_https_ota_get_image_len_read(ota_handle);
        if (total > 0) {
            int percent = read * 100 / total;
            if (percent != last_percent && cb) {
                cb(percent, "downloading");
                last_percent = percent;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        if (cb) cb(0, "OTA download failed");
        return ret;
    }

    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        if (cb) cb(0, "OTA finish failed");
        return ret;
    }

    ESP_LOGI(TAG, "OTA success, restarting...");
    if (cb) cb(100, "OTA success");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}
```

### Step 5: MQTT 指令处理

**修改 `mqtt_client.c`**，增加 OTA 指令解析。

在 `parse_function_command()` 函数中，增加对 OTA 升级指令的处理。但实际上，OTA 指令不是走 `function/get` 主题，而是走 `upgrade/set` 主题，所以需要在 MQTT 订阅和回调中增加这个主题。

**修改订阅主题**（`mqtt_app_start` 中）：

```c
// 原有订阅
esp_mqtt_client_subscribe(client,
    "/" FB_PRODUCT_ID "/" FB_DEVICE_NUM "/function/get", 0);

// 新增 OTA 升级主题订阅
esp_mqtt_client_subscribe(client,
    "/" FB_PRODUCT_ID "/" FB_DEVICE_NUM "/upgrade/set", 0);
```

**新增 OTA 消息处理函数**：

```c
static void handle_ota_upgrade(cJSON *root)
{
    cJSON *otaUrl = cJSON_GetObjectItem(root, "otaUrl");
    cJSON *firmwareVersion = cJSON_GetObjectItem(root, "firmwareVersion");
    cJSON *messageId = cJSON_GetObjectItem(root, "messageId");

    if (!otaUrl || !cJSON_IsString(otaUrl)) {
        ESP_LOGE(TAG, "OTA: missing otaUrl");
        return;
    }

    const char *url = otaUrl->valuestring;
    const char *target_ver = firmwareVersion ? firmwareVersion->valuestring : "unknown";
    const char *msg_id = messageId ? messageId->valuestring : "";

    ESP_LOGI(TAG, "OTA upgrade request: url=%s, version=%s", url, target_ver);

    /* 检查是否需要升级 */
    if (!ota_app_need_upgrade(target_ver)) {
        ESP_LOGI(TAG, "Already at version %s, no upgrade needed", target_ver);
        /* 回复平台：无需升级 */
        send_ota_reply(msg_id, 200, "Already up to date");
        return;
    }

    /* 回复平台：收到升级指令，开始升级 */
    send_ota_reply(msg_id, 200, "Starting upgrade");

    /* 启动 OTA（在新任务中执行，避免阻塞 MQTT 回调） */
    /* 将 URL 保存到静态变量，通过事件队列触发 */
    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    system_msg_t msg = {
        .type = MSG_OTA_START,
    };
    state_machine_send_event(&msg);
}
```

**新增 OTA 回复函数**：

```c
static void send_ota_reply(const char *message_id, int code, const char *msg)
{
    char topic[128];
    snprintf(topic, sizeof(topic),
             "/%s/%s/upgrade/reply", FB_PRODUCT_ID, FB_DEVICE_NUM);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "messageId", message_id);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", msg);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);

    free(payload);
    cJSON_Delete(root);
}
```

### Step 6: 状态机集成

**修改 `lamp_types.h`**，增加 OTA 事件类型和命令：

```c
typedef enum {
    CMD_POWER_TOGGLE = 0,
    CMD_LAMP_TOGGLE,
    CMD_SET_BRIGHTNESS,
    CMD_SET_MODE,
    CMD_OTA_START,              // 新增
} command_id_t;

typedef enum {
    MSG_BUTTON = 0,
    MSG_COMMAND,
    MSG_TIMER,
    MSG_MQTT,
    MSG_OTA_START,              // 新增
} message_type_t;
```

**修改 `state_machine.c`**，处理 OTA 事件：

```c
case MSG_OTA_START:
{
    ESP_LOGI(TAG, "Starting OTA upgrade...");
    /* 通知用户灯开始升级（可选） */
    indicator_set_all(true);  // 全亮表示升级中

    /* 执行 OTA */
    ota_progress_cb_t cb = [](int percent, const char *msg) {
        ESP_LOGI(TAG, "OTA progress: %d%% - %s", percent, msg);
        /* 可通过 MQTT 上报进度 */
    };

    esp_err_t ret = ota_app_start(s_ota_url, cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        indicator_set_all(false);
    }
    break;
}
```

---

## 8. 调试与验证

### 8.1 调试顺序

```
Step 1: MCU OTA 基础验证
  ├── 配置分区表，编译通过
  ├── 用本地 HTTP 服务器放固件.bin
  ├── 串口手动触发 OTA 下载
  └── 验证：固件能下载、写入、重启

Step 2: MQTT 指令接收验证
  ├── 用 MQTTX 手动发布 OTA 指令到 upgrade/set 主题
  ├── 看串口日志是否收到
  └── 验证：设备是否开始下载固件

Step 3: 后端触发验证
  ├── Web 端上传固件
  ├── Web 端创建升级任务
  ├── 看后端日志是否发送了 MQTT 消息
  └── 验证：设备是否收到并开始升级

Step 4: 端到端验证
  ├── Web 端查看升级状态
  ├── 设备升级后上报新版本
  └── 验证：Web 端状态是否更新为"成功"
```

### 8.2 用 MQTTX 模拟后端发送 OTA 指令

在实现 MCU 端后端联调前，可以用 MQTTX 先测试：

**连接配置：**
- Broker: `wss://fleetbee.top/mqtt` 或你自己的 MQTT 服务器
- 用户名: `fastbee`
- 密码: 你的 token

**发布 OTA 指令：**

| 参数 | 值 |
|------|-----|
| Topic | `/{productId}/{serialNumber}/upgrade/set` |
| 示例 | `/136/D1088N947N1G/upgrade/set` |
| QoS | 1 |
| Payload | 见下方 JSON |

**Payload JSON：**
```json
{
    "otaId": 1,
    "otaUrl": "http://192.168.1.100:8080/profile/iot/firmware.bin",
    "firmwareVersion": "1.1",
    "firmwareName": "test_firmware",
    "seqNo": "001",
    "productId": 136,
    "pushType": 0,
    "serialNumber": "D1088N947N1G",
    "taskId": 1,
    "messageId": "1234567890"
}
```

> 注意：`otaUrl` 必须是设备能访问的 HTTP 地址。如果是局域网测试，用你电脑的局域网 IP。

### 8.3 固件文件准备

用 ESP-IDF 编译项目会生成 `build/lamp_controller.bin`，把它放到一个 HTTP 服务器上：

```bash
# Python 简单 HTTP 服务器
cd build
python -m http.server 8080
# 固件可通过 http://你的IP:8080/lamp_controller.bin 访问
```

### 8.4 常见问题

| 问题 | 排查方法 |
|------|---------|
| 设备收不到 OTA 指令 | 用 MQTTX 订阅 `+/+/upgrade/set` 确认后端是否发送 |
| 固件下载失败 | 检查 otaUrl 是否能访问，防火墙是否放行 |
| OTA 写入失败 | 检查分区表是否正确，Flash 大小是否够 |
| 升级后启动旧固件 | 检查 `esp_ota_set_boot_partition` 是否调用 |
| 无限重启循环 | 检查新固件是否有效，可能需要回滚机制 |
| 后端状态不变 | 确认设备是否回复了 `upgrade/reply` 主题 |

### 8.5 关键日志点（MCU 串口）

```
OTA: Starting OTA from: http://192.168.1.100:8080/firmware.bin
OTA: Downloading... 10%
OTA: Downloading... 50%
OTA: Downloading... 100%
OTA: OTA success, restarting...
# 重启后
mqtt: Connected, reporting info with version 1.1
```

---

## 总结：实现完成

| 任务 | 状态 | 修改文件 |
|------|------|---------|
| 分区表 + 编译配置 | 已完成 | partitions.csv, sdkconfig.defaults, CMakeLists.txt |
| OTA 模块实现 | 已完成 | ota_app.c, ota_app.h |
| MQTT 指令处理 | 已完成 | mqtt_client.c (订阅 + 解析 + 回复) |
| 状态机集成 | 已完成 | state_machine.c (handle_ota_start), lamp_types.h |
| 指示灯控制 | 已完成 | indicator.c/.h (indicator_set_all) |
| OTA 回滚验证 | 已完成 | main.c (启动时标记 valid) |
| HTTPS 证书支持 | 已完成 | sdkconfig.defaults (证书包), ota_app.c (crt_bundle_attach) |
| 主题格式兼容 | 已完成 | mqtt_client.c (v2.0 + v1.x 双格式) |
| 相对 URL 拼接 | 已完成 | ota_app.c (FB_OTA_BASE_URL), lamp_types.h |

后端和前端无需改动。MCU 端 OTA 功能已全部实现，可进行端到端测试。

### 测试步骤

1. **编译固件**: `idf.py build` — 确认编译通过
2. **烧录固件**: `idf.py flash` — 首次烧录到 factory 分区
3. **配网 + 连接**: 设备完成 AP 配网，连接 WiFi + MQTT
4. **Web 端上传固件**: 在 FastBee 平台上传新的 .bin 固件文件
5. **创建升级任务**: 在固件详情页新增任务，选择目标设备
6. **观察升级过程**: 串口日志查看 OTA 下载进度，指示灯全亮表示升级中
7. **验证升级成功**: 设备重启后上报新版本号，Web 端状态变为"成功"

### 关键配置项

| 配置 | 值 | 位置 |
|------|-----|------|
| FB_FIRMWARE_VERSION | "2.0" | lamp_types.h |
| FB_OTA_BASE_URL | "https://www.fleetbee.top/prod-api" | lamp_types.h |
| FB_MQTT_HOST | "81.71.99.53" | lamp_types.h |
| FB_PRODUCT_ID | "136" | lamp_types.h |
| OTA 分区 | factory(1MB) + ota_0(1MB) + ota_1(1MB) | partitions.csv |
| Stack 大小 | 8192 bytes | state_machine.c |
| HTTPS 证书 | ESP-IDF 证书包 | sdkconfig.defaults + ota_app.c |

---

> 文档更新日期：2026-09-09
> 项目：FastBee IoT + ESP32-C3 大路灯控制板
