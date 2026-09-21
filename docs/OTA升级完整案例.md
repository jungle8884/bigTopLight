# ESP32-C3 + FastBee 平台 OTA 升级完整案例

> 项目：XS-XLD5 灯控 (lamp_controller) | 芯片：ESP32-C3 | 框架：ESP-IDF v5.4.4
> 平台：FastBee（在线版 https://www.fleetbee.top）
> 实测验证：v4.0 (ota_1) → v5.0 (ota_0) 升级成功，双向分区切换均已打通
> 整理日期：2026-09-20

---

## 一、整体流程总览

```
┌─────────────────── 服务端 (FastBee 平台) ───────────────────┐
│                                                              │
│  ① 固件管理: 上传 .bin (版本号须与 PROJECT_VER 一致)          │
│  ② 设备详情页 → OTA升级 → 检查更新                            │
│  ③ 平台通过 MQTT 向 /{productId}/{deviceNum}/ota/get 发布:    │
│     {"version":"5.0","downloadUrl":"https://.../xxx.bin"}    │
└──────────────────────────────┬───────────────────────────────┘
                               │ MQTT 下发
┌──────────────────────────────▼───────────────────────────────┐
│                    嵌入式端 (ESP32-C3)                        │
│                                                              │
│  ④ MQTT 收到 ota/get 消息 → parse_ota_command 解析            │
│  ⑤ 版本比对: 目标 5.0 ≠ 当前 4.0 → 需要升级                   │
│  ⑥ URL 修复: localhost 等不可用 host 用 FB_OTA_BASE_URL 重拼  │
│  ⑦ SNTP 校时等待 (HTTPS 证书校验的前提)                       │
│  ⑧ 上报 {progress:0, status:2}                               │
│  ⑨ esp_https_ota: begin → perform(写 ota_0) → finish         │
│     每 10% 上报进度 (9%→19%→...→99%)                          │
│  ⑩ set_boot_partition 切换启动分区 (finish 未切时手动兜底)     │
│  ⑪ 上报 {progress:100, status:3} → 延时重启                   │
│                                                              │
│  ⑫ 重启后 bootloader 读 otadata → 启动新分区 (PENDING_VERIFY) │
│  ⑬ 新固件首启: esp_ota_mark_app_valid_cancel_rollback()       │
│  ⑭ 设备重新上线, 上报 firmwareVersion=5.0                    │
└──────────────────────────────────────────────────────────────┘
```

---

## 二、服务端操作步骤（FastBee 平台）

### 2.1 上传固件

1. 平台首页 → **固件管理** → 新建固件
2. 上传编译产物 `build/lamp_controller.bin`
3. **版本号必须与固件内的 `PROJECT_VER` 一致**（本例为 `5.0`）
   - 不一致的后果：设备上报的版本永远匹配不上，平台会**无限重复下发升级指令**

### 2.2 下发升级

- 设备详情页 → **OTA升级** 栏 → 显示当前 `Version 5` → 点 **检查更新**
- 若设备版本 < 固件库最新版本，平台立即通过 MQTT 推送升级指令

### 2.3 平台实际下发的 MQTT 报文（实测抓取）

| 项目 | 值 |
|---|---|
| Topic | `/136/DDCDA0C87D20C/ota/get` |
| Payload | `{"version":5.0,"downloadUrl":"https://www.fleetbee.top/prod-api/profile/iot/1/2026-0920-155053.bin"}` |
| 特点 | Web 端"检查更新"方式**不带 taskId**（走 OTA 任务列表下发才带） |

> 主题格式注意：FastBee 存在三套 OTA 主题并存（v2.0 `/{deviceNum}/http/upgrade/set`、v1.x `/{productId}/{deviceNum}/upgrade/set`、Web 端实际使用的 `/{productId}/{deviceNum}/ota/get`）。**设备端三套都要订阅**，否则收不到指令。

---

## 三、嵌入式端实现（C:\Users\jungle\test）

### 3.1 分区表设计（partitions.csv，4MB Flash）

```csv
# Name,     Type, SubType,  Offset,   Size,    Flags
nvs,         data, nvs,     0x9000,   0x6000,
phy_init,    data, phy,     0xf000,   0x1000,
otadata,     data, ota,     0x10000,  0x2000,
ota_0,       app,  ota_0,   0x20000,  0x140000,
ota_1,       app,  ota_1,   0x160000, 0x140000,
```

设计要点：

| 要点 | 说明 |
|---|---|
| 无 factory 分区 | 全部用 ota_0/ota_1 双分区 + otadata 交替切换 |
| 64KB 对齐 | app 分区 offset 必须 0x10000 对齐，否则 gen_esp32part.py 报错 |
| otadata=0x10000+0x2000 | 导致 ota_0 只能从 0x20000 起，0x12000~0x20000 是对齐代价 |
| 每分区 1.25MB | 本项目固件约 1.1MB，余量充足 |

otadata 机制：双 sector 各记录一个 sequence，**谁大启动谁**；每次 OTA 写新 seq 并置为 `PENDING_VERIFY` 状态，等待新固件确认。

### 3.2 版本号机制（单一来源）

```
顶层 CMakeLists.txt: set(PROJECT_VER "5.0")  ← 唯一写死的地方
        ↓ 编译期写进镜像元数据 esp_app_desc
运行时: ota_app_get_version() = esp_app_get_description()->version
        ↓
MQTT 设备信息上报 / OTA 版本比对 / 启动横幅，全部读这一处
```

**好处**：镜像元数据和上报平台的版本永远一致，不存在两处宏漂移；升级后版本号随固件自动变，不需要额外存储到 NVS。

### 3.3 MQTT 指令接收（mqtt_client.c）

```c
/* 连接后订阅 3 套 OTA 主题, 兼容平台不同版本的下发通道 */
esp_mqtt_client_subscribe(s_client, s_topic_upgrade_set, 1);     // v2.0: /{deviceNum}/http/upgrade/set
esp_mqtt_client_subscribe(s_client, s_topic_upgrade_set_v1, 1);  // v1.x: /{productId}/{deviceNum}/upgrade/set
esp_mqtt_client_subscribe(s_client, s_topic_ota_get, 1);         // Web:  /{productId}/{deviceNum}/ota/get
```

`parse_ota_command()` 对 JSON 字段做了多兼容：

| 字段 | 兼容写法 |
|---|---|
| 下载地址 | `otaUrl` / `downLoadUrl` / `downloadUrl` / `url` |
| 目标版本 | `firmwareVersion` / `version` |
| 任务 ID | `taskId`（可为空） |

收到指令先做版本比对，避免重复升级：

```c
bool ota_app_need_upgrade(const char *target_version) {
    if (!target_version || target_version[0] == '\0') return true;  // 无版本号默认升级
    return strcmp(target_version, ota_app_get_version()) != 0;
}
```

### 3.4 OTA 执行核心（ota_app.c）

**步骤 1：URL 修复**。自建 FastBee 按"浏览器访问地址"生成下载链接，可能是 `localhost`/`127.0.0.1`，设备解析不了 → 只取 path 用编译期 `FB_OTA_BASE_URL` 重拼；完整可用 URL 原样使用。⚠ 不能无条件重拼 host：在线平台 context-path 是 `prod-api`、自建是 `dev-api`，无脑重拼会拼出 `/prod-api/prod-api/...` 双前缀 → 404。

**步骤 2：SNTP 校时**（HTTPS 前提）。设备刚上电 `time()` 从 1970 起算，mbedTLS 校验证书有效期必失败（`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`）。main.c 在 WiFi 连上后异步启动 SNTP（`ntp.aliyun.com`，`CONFIG_LWIP_SNTP_MAX_SERVERS=1` 只能配一个），OTA 建链前用 `wait_for_time_sync(15000)` 等时间有效：

```
I (1071153) OTA: System time ok: 2026-09-20 15:51:38
I (1072039) esp-x509-crt-bundle: Certificate validated
```

**步骤 3：下载写入**。`esp_https_ota_begin → perform 循环 → finish`，关键配置：

```c
esp_https_ota_config_t ota_config = {
    .http_config = &config,
    /* ⚠ 本项目最关键的一个配置 (2026-09-20 实测踩坑):
     * partial_http_download=true 时 begin() 先发 HTTP HEAD 请求,
     * fleetbee.top 对 HEAD 不返回 Content-Length → image_length=0
     * → perform() 永远进不了 SUCCESS → finish() 静默跳过 set_boot
     *   却返回 ESP_OK → otadata 没写 → 重启回旧固件且无任何报错。
     * 关掉后 image_length 从 GET 响应取, 状态机正常走到 SUCCESS。 */
    .partial_http_download = false,
};
```

下载期间每 10% 上报一次进度（实测日志 `9%→19%→...→99%`，固件 1148768 字节，全程约 88 秒）。

**步骤 4：切换启动分区 + 兜底**。finish 之后手动确认/补刀：

```c
ret = esp_https_ota_finish(ota_handle);
/* ... */
const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
const esp_partition_t *boot_now = esp_ota_get_boot_partition();
if (boot_now && boot_now->address == update_part->address) {
    /* finish 已正常切换, 不重复做 image_validate (省约 200ms) */
} else {
    ret = esp_ota_set_boot_partition(update_part);   /* 手动兜底 */
    ESP_LOGI(TAG, "Manual esp_ota_set_boot_partition(%s): %s", ...);
}
```

- `esp_ota_get_next_update_partition(NULL)` 基于 **running 分区**取对端（不是读 otadata），running=ota_1 时必然返回刚烧录的 ota_0，语义正确
- 重复 set_boot 幂等无害，但会重复校验镜像；所以优化为**条件触发**

**步骤 5：上报成功并重启**。

```
I (1160628) OTA: Manual esp_ota_set_boot_partition(ota_0): ESP_OK
I (1160628) OTA: After set_boot: boot=ota_0 running=ota_1
I (1160632) MQTT: OTA reply: {"progress":100,"version":"5.0","status":3}
I (1162634) OTA: Restarting in 1 second...
```

### 3.5 重启后：bootloader 与防回滚

bootloader 读 otadata（两个 seq 都为 0x00000003 时按 slot 顺序取）→ 选中 ota_0 并标记 `PENDING_VERIFY`：

```
D (200) boot: otadata[0]: sequence values 0x00000003
D (205) boot: otadata[1]: sequence values 0x00000003
D (214) boot: Active otadata[0]
D (217) boot: Mapping seq 2 -> OTA slot 0
D (220) boot: otadata[0] is selected as new and marked PENDING_VERIFY state
I (683)  app_init: App version: 5.0
```

新固件首启（main.c）确认有效，**取消回滚资格**：

```c
if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
}
I (817) MAIN: OTA: first boot of new firmware, marking valid
I (890) MAIN: Running partition: ota_0, firmware v5.0
```

> 回滚机制原理：`PENDING_VERIFY` 状态下**再次重启**（而非计时器）才会回滚到旧分区。新固件跑起来后主动 mark valid，之后重启不再回滚。本项目未启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 之外的看门狗式自检，如需"业务自检不过自动回滚"，把 mark valid 挪到业务初始化成功之后即可。

### 3.6 进度/状态上报协议

设备 → 平台 Topic：`/{deviceNum}/http/upgrade/reply`、`/{productId}/{deviceNum}/upgrade/reply`、`/{productId}/{deviceNum}/ota/post` 三路同发（兼容不同平台版本的接收通道）。

| status | 含义 |
|---|---|
| 2 | 升级中（下载进行时） |
| 3 | 升级成功（progress=100） |
| 4 | 升级失败 |

实测上报序列：`{progress:0,status:2}` → `{9..99,status:2}` → `{progress:100,status:3}`。

---

## 四、踩坑记录（重要度排序）

| # | 坑 | 现象 | 根因 | 修复 |
|---|---|---|---|---|
| 1 | `partial_http_download=true` | finish 返回 OK 但重启回旧固件，无任何报错 | 平台对 HTTP HEAD 不返回 Content-Length → image_length=0 → 状态机不进 SUCCESS → finish 跳过 set_boot | `.partial_http_download = false` + finish 后手动 set_boot 兜底 |
| 2 | 双重 set_boot | 升级后启动旧/空分区 | 业务代码 finish 后又调 set_boot，两次写 otadata seq 混乱 | 删除冗余调用，只留条件触发的兜底 |
| 3 | 系统时间 1970 | TLS 握手报证书校验失败 | mbedTLS 要校验证书有效期 | WiFi 连接后 SNTP 校时 + OTA 前 wait_for_time_sync |
| 4 | 分区未对齐 | 编译报 `Offset 0x12000 is not aligned to 0x10000` | otadata 结束在 0x12000，不是 64KB 边界 | ota_0 推到 0x20000 |
| 5 | 平台下发 localhost | 设备下载 404/解析失败 | 自建平台按浏览器地址生成 downloadUrl | is_unusable_host 检测 + FB_OTA_BASE_URL 重拼 |
| 6 | 版本号写死在多处 | 升级后上报版本不变 | 宏与镜像元数据漂移 | 版本号单一来源 PROJECT_VER → esp_app_desc |

---

## 五、实测完整日志时间线（v4.0 → v5.0）

| 时刻(ms) | 事件 |
|---|---|
| 1071097 | MQTT 收到 ota/get 升级指令（url + version=5.0） |
| 1071153 | 系统时间校验通过（2026-09-20 15:51:38） |
| 1072039 | HTTPS 证书校验通过 |
| 1073287 | 开始写入 ota_0 @ 0x20000 |
| 1080893~1159261 | 下载进度 9%→99%（每 10% 上报） |
| 1160405 | 下载完成，esp_image 校验 6 个 segment + hash |
| 1160628 | 手动 set_boot(ota_0)=ESP_OK，boot=ota_0 running=ota_1 |
| 1160632 | 上报 progress=100, status=3 |
| 1163688 | esp_restart，重启 |
| +645 | bootloader 读 otadata，选 ota_0，标记 PENDING_VERIFY |
| +683 | App version: 5.0（镜像元数据） |
| +817 | 首启 mark valid，取消回滚 |
| +890 | Running partition: ota_0, firmware v5.0 |
| +3735 | MQTT 重新上线，恢复订阅 |

全程从收到指令到新固件上线约 **17 秒**（下载 88 秒 + 重启验证 4 秒 + 此前等待）。

---

## 六、遗留问题（不影响 OTA，待清理）

1. **空 topic 订阅**：日志出现 4 条 `MQTT: subscribed to `（空字符串）——主题字符串初始化时序问题，部分 buffer 未填充就打印/订阅了
2. **`function parse failed:`**：收到空 payload 的 function/get 消息时解析失败告警
3. 平台上传固件版本号务必与 `PROJECT_VER` 严格一致，否则会无限重复下发

---

## 七、复现清单（新项目移植步骤）

1. 分区表：ota_0/ota_1 双 app 分区 + otadata，offset 64KB 对齐
2. 顶层 CMakeLists：`set(PROJECT_VER "x.x")` 放在 `project()` 之前；main 组件 REQUIRES 加 `esp_app_format`
3. 开启 SNTP（WiFi 连接回调里异步启动），OTA 建链前等时间有效
4. `esp_https_ota` 配置：`crt_bundle_attach` + `partial_http_download=false` + timeout≥60s
5. finish 后条件触发手动 set_boot 兜底，并打印 `boot=xxx running=xxx` 诊断行
6. 新固件 app_main 首行附近 mark valid（或按业务自检后确认）
7. MQTT 端三套 OTA 主题全订阅、回复三路同发，字段名多兼容
8. 互斥锁防并发 OTA；所有失败路径释放锁并上报 status=4
