# FastBee AP 配网流程详解

> 涉及模块：APP (`deviceAdd.vue`) + MCU (`wifi_app.c`) + 后端 (SpringBoot)
> 适用项目：XS-XLD5 V3.0 大路灯控制板 / ESP32-C3 / FastBee 物联网平台

***

## 目录

- [1. 整体架构](#1-整体架构)

- [2. Phase 1 — 准备阶段：设备启动 AP 热点](#2-phase-1--准备阶段设备启动-ap-热点)

- [3. Phase 2 — APP 发现设备](#3-phase-2--app-发现设备)

- [4. Phase 3 — 下发配网参数](#4-phase-3--下发配网参数)

- [5. Phase 4 — 连接 WiFi + MQTT 认证上线](#5-phase-4--连接-wifi--mqtt-认证上线)

- [6. 关键数据存储](#6-关键数据存储)

- [7. 完整时序总览](#7-完整时序总览)

- [8. 多设备配网（微信小程序模式）](#8-多设备配网微信小程序模式)

- [9. 重新配网](#9-重新配网)

- [10. 涉及的关键源码文件](#10-涉及的关键源码文件)

***

## 1. 整体架构

配网分为 4 个阶段，APP 和 MCU 之间通过 **HTTP 直连**（手机连设备热点），MCU 和后端之间通过 **MQTT 认证**上线。

```
APP (deviceAdd.vue)          MCU (wifi_app.c)           后端 (SpringBoot)
     │                            │                            │
     │      HTTP 直连 192.168.4.1  │                            │
     │◄──────────────────────────►│                            │
     │                            │      MQTT CONNECT           │
     │                            │───────────────────────────►│
     │                            │      认证 + 自动注册         │
     │                            │◄───────────────────────────►│
```

### 触发 AP 配网的条件

| 条件                               | 说明            |
| -------------------------------- | ------------- |
| NVS 中无 WiFi 凭据                   | 首次使用，最常见场景    |
| 直接连接重试超过 `WIFI_MAX_RETRY` 次仍失败   | 路由器密码变更、信号差等  |
| 外部调用 `wifi_app_start_apconfig()` | 如按键长按手动触发重新配网 |

***

## 2. Phase 1 — 准备阶段：设备启动 AP 热点

### 2.1 MCU 启动流程 (`wifi_app.c` → `wifi_app_start()`)

```
设备上电
  → storage_init()  初始化 NVS
  → wifi_app_start()
      → 检查 NVS 是否有 WiFi 凭据 (storage_load_wifi_creds)
      ├─ 有凭据 → STA 模式直接连接路由器
      │           ├─ 连接成功 → 回调启动 MQTT
      │           └─ 重试超过 5 次 → 回退 AP 模式
      └─ 无凭据 → start_ap_mode()  ← 首次使用走这里
```

### 2.2 `start_ap_mode()` 做的 3 件事

| 步骤                | 代码                                   | 说明                                     |
| ----------------- | ------------------------------------ | -------------------------------------- |
| 1. 开启热点           | `esp_wifi_set_mode(APSTA)`           | 热点名 `FastBee-XXXX`（MAC 后 4 位 hex），开放认证 |
| 2. 分配 IP          | `esp_netif_create_default_wifi_ap()` | 默认 IP `192.168.4.1`                    |
| 3. 启动 HTTP Server | `httpd_start()`                      | 端口 80，注册 3 个 URI handler               |

### 2.3 热点命名规则

```c
// 生成 AP 热点名: FastBee-XXXX (MAC 后 4 hex)
uint8_t mac[6] = {0};
esp_read_mac(mac, ESP_MAC_WIFI_STA);
char ap_ssid[32];
snprintf(ap_ssid, sizeof(ap_ssid), "FastBee-%02X%02X", mac[4], mac[5]);
```

### 2.4 HTTP Server 提供的 3 个接口

| 接口        | 方法      | 用途                                  |
| --------- | ------- | ----------------------------------- |
| `/status` | GET     | APP 轮询检测设备是否就绪，返回 200 + "AP配网已准备就绪" |
| `/config` | POST    | 接收 APP 下发的 WiFi 凭据和设备信息（核心配网接口）     |
| `/*`      | OPTIONS | CORS 预检处理（兼容 H5/浏览器）                |

### 2.5 关键代码位置

- **文件**：`main/wifi_app.c`

- **函数**：`start_ap_mode()`（约第 164 行）

- **HTTP Server 启动**：约第 197 行

***

## 3. Phase 2 — APP 发现设备

### 3.1 APP 端初始化 (`deviceAdd.vue`)

用户进入"添加设备"页面后，APP 执行以下步骤：

```
1. 获取登录用户信息 → this.form.userId = profile.userId
   ↓
2. 断开 MQTT 连接 (避免配网期间干扰)
   ↓
3. 读取本地缓存的 WiFi 信息 (如果之前保存过)
   ↓
4. discoverDevice() — 轮询检测设备
```

### 3.2 设备发现轮询 — `discoverDevice()`

```javascript
// 每 5 秒轮询一次
this.discoverTimer = setInterval(() => {
    if (this.tabIndex == 0) {
        uni.request({
            url: 'http://192.168.4.1/status',
            method: 'GET',
            timeout: 5000,
            success: res => {
                clearInterval(this.discoverTimer);
                this.step = 2;  // 进入"检测到设备"状态
                this.count = {
                    text: '已检测到设备',
                    type: 'success'
                };
            }
        });
    }
}, 5000);
```

### 3.3 前提条件

用户需要先在手机系统设置中手动连接 `FastBee-XXXX` 热点，APP 才能访问 `192.168.4.1`。

### 3.4 APP 页面步骤引导

```
步骤1: 设备进入配网模式  (设备已开启热点)
步骤2: 手动连接设备热点  (手机系统设置连热点)
步骤3: 检测设备          (轮询 /status)
步骤4: 配网结束          (下发参数后完成)
```

### 3.5 MCU 端 /status 处理

```c
static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "AP配网已准备就绪", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
```

***

## 4. Phase 3 — 下发配网参数

### 4.1 APP 端构建请求 — `getParamString()`

```javascript
getParamString () {
    let ip = 'http://192.168.4.1/config';
    let params = '?SSID=' + this.form.SSID
               + '&password=' + this.form.password
               + '&userId=' + this.form.userId;
    if (this.form.deviceNum && this.form.deviceNum != '') {
        params = params + '&deviceNum=' + this.form.deviceNum;
    }
    if (this.form.authCode && this.form.authCode != '') {
        params = params + '&authCode=' + this.form.authCode;
    }
    if (this.form.extra && this.form.extra != '') {
        params = params + '&extra=' + this.form.extra;
    }
    return ip + params;
}
```

### 4.2 APP 端发送请求 — `apConfig()`

```javascript
apConfig () {
    return new Promise((resolve, reject) => {
        uni.request({
            url: this.getParamString(),
            method: 'POST',
            timeout: 10000,
            success: res => { resolve(true); },
            fail: res => { reject(false); }
        });
    });
}
```

### 4.3 APP 端配网主流程 — `beginConfig()`

```javascript
async beginConfig () {
    // 1. 表单验证
    if (!this.$refs.form.validate()) {
        uni.$u.toast('用户编号和WIFI账号密码不能为空');
        return;
    }
    // 2. 保存WiFi信息到本地缓存
    if (this.checkboxConfigs.indexOf('remeber') != -1) {
        this.saveWifi();
    }
    // 3. 显示进度条
    this.progress = 0;
    this.showConfigProgress();
    // 4. 发送配网请求
    try {
        let result = await this.apConfig();
        if (result) {
            this.progress = 100;
            this.count.text = '配网成功，如果设备没有正常连接，请检查WIFI信息是否正确以及网络状况';
            this.step = 3;
        } else {
            this.count.text = '配网失败，请确认设备进入配网模式，并连接了该热点';
            this.step = 4;
        }
    } catch (e) {
        this.count.text = '配网失败，请确认设备进入配网模式，并连接了该热点';
        this.step = 4;
    }
}
```

### 4.4 MCU 端接收处理 — `config_handler()`

```c
static esp_err_t config_handler(httpd_req_t *req)
{
    // 1. 提取 URL query string
    char query[512] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    // 2. 解析必填参数
    char ssid[33], password[65], user_id[16];
    httpd_query_key_value(query, "SSID", ssid, sizeof(ssid));
    httpd_query_key_value(query, "password", password, sizeof(password));
    httpd_query_key_value(query, "userId", user_id, sizeof(user_id));

    // 3. 解析可选参数
    char device_num[33], auth_code[33];
    httpd_query_key_value(query, "deviceNum", device_num, sizeof(device_num));
    httpd_query_key_value(query, "authCode", auth_code, sizeof(auth_code));

    // 4. 未传 deviceNum → 用 MAC 自动生成
    if (strlen(device_num) == 0) {
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(device_num, sizeof(device_num),
                 "D%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // 5. 保存到 NVS（掉电不丢失）
    storage_save_wifi_creds(ssid, password);
    storage_save_device_info(user_id, device_num,
                             strlen(auth_code) > 0 ? auth_code : "");

    // 6. 发送 HTTP 200 响应给 APP
    httpd_resp_send(req, "设备已更新WIFI配置，开始连接WIFI...",
                    HTTPD_RESP_USE_STRLEN);

    // 7. 延迟 2 秒确保响应送达，然后切换模式
    vTaskDelay(pdMS_TO_TICKS(2000));
    httpd_stop(s_http_server);           // 关闭 HTTP Server
    s_ap_mode = false;
    esp_wifi_set_mode(WIFI_MODE_STA);    // 切换 STA
    esp_wifi_connect();                  // 连接路由器
    return ESP_OK;
}
```

### 4.5 APP 下发的参数清单

| 参数          | 必填 | 说明          | 来源                       |
| ----------- | -- | ----------- | ------------------------ |
| `SSID`      | 是  | 路由器 WiFi 名称 | 用户输入                     |
| `password`  | 是  | 路由器 WiFi 密码 | 用户输入                     |
| `userId`    | 是  | 平台用户 ID     | APP 自动获取（登录信息）           |
| `deviceNum` | 否  | 设备编号        | 用户输入/扫码，未传则 MCU 用 MAC 生成 |
| `authCode`  | 否  | 授权码         | 高级模式输入                   |
| `extra`     | 否  | 补充信息        | 高级模式输入                   |

***

## 5. Phase 4 — 连接 WiFi + MQTT 认证上线

### 5.1 MCU 端 WiFi 连接成功后的回调链

```
esp_wifi_connect() 成功
  → IP_EVENT_STA_GOT_IP 事件触发
  → wifi_event_handler() 中:
      xEventGroupSetBits(s_wifi_event_group, BIT_CONNECTED)
      s_connected_cb()  ← 回调函数
  → on_wifi_connected()  (main.c 中注册)
  → mqtt_app_start()     ← 启动 MQTT 客户端
```

### 5.2 回调注册 (`main.c`)

```c
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected, starting MQTT client...");
    mqtt_app_start();
}

// 在 app_main() 中注册
wifi_app_set_connected_callback(on_wifi_connected);
wifi_app_start();
```

### 5.3 MQTT ClientId 构建 (`mqtt_client.c`)

MCU 使用配网时保存的参数构建 MQTT ClientId：

```
ClientId 格式: S&{deviceNum}&{productId}&{userId}

示例: S&D1088N947N1G&136&1
      │  │              │   │
      │  │              │   └─ userId (配网时下发)
      │  │              └───── productId (编译时定义)
      │  └──────────────────── deviceNum (配网时下发或MAC生成)
      └──────────────────────── 认证类型 S=简单认证, E=加密认证
```

### 5.4 后端认证处理链 (SpringBoot)

```
MQTT CONNECT 到达
  → MqttConnect.handler()
      → 提取 clientId: "S&D1088N947N1G&136&1"
      → AuthService.auth(clientId, username, password)
          → clientId 不以 server/web/phone 开头 → 设备端认证
          → ToolServiceImpl.clientAuth()
              → split("&") → ["S", "D1088N947N1G", "136", "1"]
              → authType="S", deviceNum="D1088N947N1G", productId=136, userId=1
              → 查询产品认证信息 (selectProductAuthenticate)
              → 验证产品状态 = 2 (已发布)
              → simpleMqttAuthentication()
                  → 验证 username + password
                  → 处理 authCode (如果有)
                  → 查询设备是否存在
                  ├─ 存在 → 认证成功
                  └─ 不存在 → insertDeviceAuto() 自动注册设备
                      → status=3 (在线), activeTime=now
                      → 插入 iot_device 表
                      → 插入 iot_device_user 表
                  → 返回 200 OK
          → 认证通过 → CONNACK 返回设备
```

### 5.5 后端认证方式说明

| 认证类型 | ClientId 前缀 | 密码格式                                         | 适用场景       |
| ---- | ----------- | -------------------------------------------- | ---------- |
| 简单认证 | `S&`        | `mqttPassword(&authCode)`                    | 开发测试、低安全场景 |
| 加密认证 | `E&`        | AES加密后: `mqttPassword&expireTime(&authCode)` | 生产环境       |

### 5.6 认证成功后的行为

**MCU 端：**

```
MQTT_EVENT_CONNECTED
  → 订阅 /{productId}/{deviceNum}/function/get  (接收平台命令)
  → 发布 /{productId}/{deviceNum}/info/post     (设备信息, status=3 在线)
```

**后端：**

```
→ 设备状态更新为在线 (status=3)
→ 设备数据写入 Redis 缓存
→ APP 可通过 REST API 查看设备
```

### 5.7 后端认证关键源码位置

| 功能        | 文件                                                             |
| --------- | -------------------------------------------------------------- |
| MQTT 连接处理 | `fastbee-server/mqtt-broker/.../MqttConnect.java`              |
| 认证入口      | `fastbee-server/mqtt-broker/.../AuthService.java`              |
| 设备端认证     | `fastbee-service/fastbee-iot-service/.../ToolServiceImpl.java` |
| 简单认证      | `ToolServiceImpl.simpleMqttAuthentication()` (第 293 行)         |
| 加密认证      | `ToolServiceImpl.encryptAuthentication()` (第 343 行)            |
| 自动注册设备    | `DeviceServiceImpl.insertDeviceAuto()` (第 857 行)               |
| 授权码处理     | `ToolServiceImpl.authCodeProcess()` (第 492 行)                  |

***

## 6. 关键数据存储

### 6.1 MCU NVS 存储（配网参数持久化）

| NVS 键         | 内容      | 写入时机          | 读取时机     |
| ------------- | ------- | ------------- | -------- |
| WiFi SSID     | 路由器名称   | `/config` 下发时 | 每次启动     |
| WiFi Password | 路由器密码   | `/config` 下发时 | 每次启动     |
| userId        | 平台用户 ID | `/config` 下发时 | MQTT 启动时 |
| deviceNum     | 设备编号    | `/config` 下发时 | MQTT 启动时 |
| authCode      | 授权码     | `/config` 下发时 | MQTT 启动时 |

### 6.2 后端数据库（设备注册信息）

| 表                 | 关键字段                                               | 说明            |
| ----------------- | -------------------------------------------------- | ------------- |
| `iot_device`      | serial\_number, product\_id, status, active\_time  | 设备主表，认证时自动创建  |
| `iot_device_user` | device\_id, user\_id, is\_owner                    | 设备-用户关联，所有者关系 |
| `iot_product`     | mqtt\_account, mqtt\_password, vertificate\_method | 产品认证配置        |

### 6.3 设备状态定义

| 状态值 | 含义  | 说明         |
| --- | --- | ---------- |
| 1   | 未激活 | 手动添加但未上线   |
| 2   | 禁用  | 管理员手动禁用    |
| 3   | 在线  | 设备已连接 MQTT |
| 4   | 离线  | 曾在线但当前断开   |

### 6.4 APP 本地缓存

| 键               | 内容      | 用途            |
| --------------- | ------- | ------------- |
| `WIFI_SSID`     | WiFi 名称 | 记住密码，下次配网自动填充 |
| `WIFI_PASSWORD` | WiFi 密码 | 记住密码，下次配网自动填充 |

***

## 7. 完整时序总览

```
时间轴 ──────────────────────────────────────────────────────────────→

[设备上电]                                    [MQTT上线]
    │                                              │
    ▼                                              ▼
MCU: wifi_app_start()                    MCU: mqtt_app_start()
    │                                        │
    ├─ 无凭据 → start_ap_mode()              ├─ ClientId: S&deviceNum&productId&userId
    │  ├─ 热点 FastBee-XXXX                  │
    │  └─ HTTP :80                           │
    │                                        │
APP: 进入 deviceAdd.vue              后端: MQTT认证
    │                                        │
    ├─ 手机连 FastBee-XXXX 热点              ├─ ToolServiceImpl.clientAuth()
    │                                        ├─ 解析 clientId 4段
    ├─ 轮询 GET /status (每5秒)              ├─ 查产品认证配置
    │  ← 200 OK                              ├─ 验证账号密码
    │                                        ├─ 处理 authCode
    ├─ POST /config?SSID&password&userId     ├─ 设备不存在 → 自动注册
    │  ← 200 "开始连接WiFi"                  │
    │                                        └─ CONNACK → 认证成功
MCU: 保存NVS → 切STA → 连路由器                 │
    │                                        │
    ├─ GOT_IP → 回调                  MCU: 订阅 function/get
    │                                 └→ 发布 info/post (status=3)
    ▼
[配网完成，设备在线]
```

***

## 8. 多设备配网（微信小程序模式）

APP 还支持微信小程序多设备配网，与单设备流程的区别：

| 对比项  | 单设备             | 多设备(小程序)                                |
| ---- | --------------- | --------------------------------------- |
| 热点连接 | 手机系统设置手动连       | `wx.connectWifi()` API 自动连              |
| 设备发现 | 轮询 /status      | `wx.getWifiList()` 扫描所有热点               |
| 配网对象 | 单个设备            | 批量选择多个设备热点                              |
| 配网流程 | 逐个 POST /config | 循环连接+POST /config                       |
| 完成后  | 手动重连WiFi        | `wx.stopWifi()` + `wx.startWifi()` 自动恢复 |

### 8.1 多设备配网流程

```javascript
async beginConfigInWeChart () {
    // 1. 验证表单
    // 2. 验证已选择设备热点
    // 3. 循环处理每个选中的设备
    for (let i = 0; i < that.selectedWifiList.length; i++) {
        // 3.1 微信API连接设备热点
        await that.connectWifiInWeChat(ssid, '');
        // 3.2 POST /config 下发配网参数
        let apResult = await that.apConfig();
        // 3.3 更新进度条
    }
    // 4. 配网完成后恢复手机WiFi
    await that.stopWifiInWeChat();
    await that.startWifiInWeChat();
}
```

### 8.2 微信小程序 WiFi API

| API                                | 用途           |
| ---------------------------------- | ------------ |
| `wx.startWifi()`                   | 初始化 WiFi 模块  |
| `wx.getWifiList()`                 | 获取附近 WiFi 列表 |
| `wx.connectWifi({SSID, password})` | 连接指定 WiFi    |
| `wx.getConnectedWifi()`            | 获取当前连接的 WiFi |
| `wx.stopWifi()`                    | 停止 WiFi 模块   |

> **注意：** 微信小程序使用 WiFi API 需要先调用 `uni.getLocation()` 获取定位授权。

***

## 9. 重新配网

MCU 端支持手动触发重新配网，可用于按键长按等场景。

### 9.1 重新配网函数

```c
void wifi_app_start_apconfig(void)
{
    ESP_LOGI(TAG, "Manual AP config trigger");
    // 1. 停止 HTTP Server (如果已运行)
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    // 2. 断开当前 WiFi 连接
    esp_wifi_disconnect();
    // 3. 清除 NVS 中的旧凭据
    storage_clear_wifi();
    // 4. 进入 AP 模式
    start_ap_mode();
}
```

### 9.2 重新配网后的流程

设备清除旧凭据后回到 Phase 1 的 AP 热点等待状态，用户可以重新通过 APP 配网。

***

## 10. 涉及的关键源码文件

### 10.1 MCU 端 (ESP32-C3)

| 文件                   | 功能                               |
| -------------------- | -------------------------------- |
| `main/wifi_app.c`    | WiFi 连接管理、AP 模式、HTTP Server、配网接口 |
| `main/wifi_app.h`    | WiFi 模块接口定义                      |
| `main/storage.c`     | NVS 存储（WiFi 凭据、设备信息）             |
| `main/storage.h`     | 存储模块接口定义                         |
| `main/mqtt_client.c` | MQTT 客户端（认证、订阅、上报）               |
| `main/main.c`        | 应用入口，注册 WiFi 连接成功回调              |

### 10.2 APP 端 (uni-app)

| 文件                                               | 功能            |
| ------------------------------------------------ | ------------- |
| `wumei-smart-app/pagesA/list/home/deviceAdd.vue` | 配网页面（单设备+多设备） |

### 10.3 后端 (SpringBoot)

| 文件                                                | 功能            |
| ------------------------------------------------- | ------------- |
| `fastbee-server/mqtt-broker/.../MqttConnect.java` | MQTT 连接处理     |
| `fastbee-server/mqtt-broker/.../AuthService.java` | MQTT 认证入口     |
| `fastbee-service/.../ToolServiceImpl.java`        | 设备认证逻辑（简单/加密） |
| `fastbee-service/.../DeviceServiceImpl.java`      | 设备自动注册        |
| `fastbee-open-api/.../DeviceController.java`      | 设备管理 REST API |
| `fastbee-common/.../ProductAuthConstant.java`     | 认证常量定义        |

***

> 文档生成日期：2026-09-07
> 项目版本：XS-XLD5 V3.0

