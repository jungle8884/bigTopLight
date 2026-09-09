# uni-app 项目阅读指南 — 从 Vue 基础到 OTA 调试

> 项目：wumei-smart-app (FastBee / JungleLink)
> 目标：理解项目结构 → 掌握核心模块 → 配合后端+MCU 调试 OTA

***

## 目录

- [1. 先搞懂：Vue 基础 vs uni-app 特有](#1-先搞懂vue-基础-vs-uni-app-特有)

- [2. 项目整体架构](#2-项目整体架构)

- [3. 核心文件阅读顺序（推荐）](#3-核心文件阅读顺序推荐)

- [4. 项目核心概念](#4-项目核心概念)

- [5. 设备详情页 — 理解 MQTT 实时通信](#5-设备详情页--理解-mqtt-实时通信)

- [6. OTA 升级完整链路](#6-ota-升级完整链路)

- [7. OTA 调试思路](#7-ota-调试思路)

- [8. 常见问题与技巧](#8-常见问题与技巧)

***

## 1. 先搞懂：Vue 基础 vs uni-app 特有

你已经学完 Vue 基础，下面这些知识**直接能用**：

| Vue 知识                                    | 在本项目中的用法                                       |
| ----------------------------------------- | ---------------------------------------------- |
| 模板语法 `{{ }}`、`v-bind`、`v-on`              | 所有 `.vue` 文件的 template 部分                      |
| `v-model` 双向绑定                            | 表单输入、uView 组件                                  |
| `v-if` / `v-for`                          | 条件渲染、列表渲染                                      |
| `data` / `methods` / `computed` / `watch` | 组件选项                                           |
| 生命周期 `created` / `mounted` / `destroyed`  | 对应 uni-app 的 `onLoad` / `onReady` / `onUnload` |
| 组件 props / emit                           | 父子组件通信                                         |
| Vuex 状态管理                                 | `store/index.js`                               |
| 全局混入 mixin                                | `$u.mixin.js`（uView 提供的）                       |

### uni-app 特有的概念（需要补充学习）

| 概念                         | 说明                                                      | 类比 Vue 的什么                                   |
| -------------------------- | ------------------------------------------------------- | -------------------------------------------- |
| **pages.json**             | 页面路由 + 全局样式 + TabBar 配置                                 | 相当于 `vue-router` + 全局样式配置                    |
| **manifest.json**          | 应用配置（各平台打包参数）                                           | 相当于 `vue.config.js` + 多端配置                   |
| **分包** (`pagesA`/`pagesB`) | 把页面拆成多个包，按需加载                                           | 相当于 Vue 的路由懒加载                               |
| **生命周期钩子**                 | `onLoad` / `onShow` / `onReady` / `onUnload`            | 类似 `created` / `mounted` 等，但更丰富              |
| **uni API**                | `uni.request` / `uni.setStorageSync` / `uni.navigateTo` | 相当于 `axios` / `localStorage` / `router.push` |
| **条件编译**                   | `#ifdef MP-WEIXIN` / `#ifndef H5`                       | 多端兼容的语法糖                                     |
| **uView UI**               | 第三方 UI 组件库                                              | 相当于 Element UI / Vant                        |

### 快速上手：记住这 5 个 uni API

```javascript
// 1. 页面跳转（相当于 router.push）
uni.navigateTo({ url: '/pagesA/list/home/deviceAdd' })
uni.navigateBack({ delta: 1 })   // 返回上一页
uni.switchTab({ url: '/pages/tabBar/home/index' })  // 切 TabBar

// 2. 网络请求（相当于 axios）
uni.request({
    url: 'https://api.example.com/list',
    method: 'GET',
    success: res => { console.log(res.data) },
    fail: err => { console.error(err) }
})

// 3. 本地存储（相当于 localStorage）
uni.setStorageSync('key', value)   // 存
uni.getStorageSync('key')           // 取

// 4. 提示框（相当于 this.$message）
uni.showToast({ title: '成功', icon: 'success' })

// 5. 获取系统信息
uni.getSystemInfoSync()  // 获取屏幕尺寸、平台等
```

***

## 2. 项目整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        APP 层 (uni-app)                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────┐  │
│  │   页面    │  │  组件    │  │  API层   │  │  工具层     │  │
│  │ pages/   │  │components│  │  apis/   │  │  common/    │  │
│  │ pagesA/  │  │          │  │          │  │  mqttTool.js │  │
│  │ pagesB/  │  │          │  │          │  │  bus.js      │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬──────┘  │
│       │             │             │                │         │
│       └─────────────┴──────┬──────┴────────────────┘         │
│                             │                                  │
│                      ┌──────┴──────┐                           │
│                      │   Vuex      │  (token + 用户信息)       │
│                      │  store/     │                           │
│                      └─────────────┘                           │
└──────────────────────────────┬───────────────────────────────┘
                               │
                     HTTP + MQTT (wss)
                               │
┌──────────────────────────────┴───────────────────────────────┐
│                     后端 (SpringBoot)                          │
│   REST API + MQTT Broker + 设备管理 + 物模型 + OTA固件管理      │
└──────────────────────────────┬───────────────────────────────┘
                               │
                            MQTT
                               │
┌──────────────────────────────┴───────────────────────────────┐
│                      MCU (ESP32-C3)                           │
│   WiFi + MQTT客户端 + 状态机 + PWM调光 + OTA升级               │
└───────────────────────────────────────────────────────────────┘
```

### 项目目录速查表

```
wumei-smart-app/
├── main.js                    ← 入口文件（全局注入）
├── App.vue                    ← 根组件
├── pages.json                 ← 【重点】路由配置（所有页面路径）
├── manifest.json              ← 应用配置
├── env.config.js              ← 【重点】环境配置（API地址 + MQTT地址）
│
├── pages/                     ← 主包页面（登录 + TabBar）
│   ├── tabBar/home/index.vue  ← 【重点】首页（设备列表）
│   └── login/index.vue        ← 登录页
│
├── pagesA/                    ← 分包A（设备相关页面）
│   └── home/
│       ├── device/index.vue   ← 【重点】设备详情页（MQTT通信核心）
│       └── device/status/     ← 设备运行状态展示
│
├── pagesB/                    ← 分包B（用户/设置相关）
│
├── apis/                      ← 【重点】API 封装层
│   ├── http.api.js            ← API 集中挂载
│   ├── http.interceptor.js    ← HTTP 拦截器
│   └── modules/
│       ├── device.js          ← 【重点】设备 API
│       └── common.js          ← 登录/用户信息 API
│
├── common/                    ← 【重点】工具类
│   ├── mqttTool.js            ← 【重点】MQTT 客户端封装
│   ├── bus.js                 ← 全局事件总线
│   └── tools.js               ← 工具函数
│
├── store/                     ← Vuex 状态管理
│   └── index.js               ← token + profile
│
├── components/                ← 公共组件
└── uni_modules/               ← 第三方插件（uView 等）
```

***

## 3. 核心文件阅读顺序（推荐）

按这个顺序读，从简单到复杂，逐步建立全局认知：

### 第 1 步：环境配置（5 分钟）

**文件**：`env.config.js`

```javascript
// 知道这两个地址就够了
baseUrl: 'https://fleetbee.top/prod-api/'   // HTTP API 地址
mqttUrl: 'wss://fleetbee.top/mqtt'           // MQTT WebSocket 地址
```

### 第 2 步：路由配置（10 分钟）

**文件**：`pages.json`

- 找到所有页面的路径

- 找到 TabBar 的 5 个页面

- 找到设备详情页、设备添加页的路径

- 理解分包概念（`pagesA`、`pagesB`）

### 第 3 步：入口文件（10 分钟）

**文件**：`main.js`

看清楚全局注入了什么：

- `$mqttTool` — MQTT 工具

- `$bus` — 事件总线

- `$api` — API 模块

- `$u` — uView UI

- Vuex 混入

### 第 4 步：HTTP 拦截器（10 分钟）

**文件**：`apis/http.interceptor.js`

理解：

- 请求拦截：怎么加 token 的

- 响应拦截：怎么处理错误的

- loading 怎么控制的

### 第 5 步：API 封装（15 分钟）

**文件**：`apis/http.api.js` + `apis/modules/device.js`

理解：

- API 怎么组织的（按模块）

- 怎么调用的（`this.$api.device.xxx()`）

- 重点看 `device.js` 里的接口

### 第 6 步：首页（20 分钟）

**文件**：`pages/tabBar/home/index.vue`

理解：

- 设备列表怎么加载的

- 下拉刷新、上拉加载

- 点击设备跳转到详情页

### 第 7 步：设备详情页（重点，30 分钟）

**文件**：`pagesA/home/device/index.vue`

这是最核心的页面，理解了它就理解了一半的业务：

- 怎么获取设备详情

- MQTT 怎么连接、订阅、接收消息

- 物模型数据怎么展示

### 第 8 步：MQTT 工具（20 分钟）

**文件**：`common/mqttTool.js`

理解：

- 怎么连接 MQTT 服务器

- 怎么订阅/取消订阅主题

- 怎么发布消息

- 重连机制

***

## 4. 项目核心概念

### 4.1 物模型（Things Model）

这是 IoT 项目最核心的概念，一定要理解：

```
一个产品（Product）有多个物模型（ThingsModel）
物模型分三类：
  ├── 属性 (property)   — 设备的状态，如亮度、温度、开关状态
  ├── 功能 (function)   — 可以下发的指令，如开关灯、调节亮度
  └── 事件 (event)      — 设备主动上报的事件，如告警、触发

每个物模型有一个唯一的 identifier（标识符），比如：
  - bright_upper  — 上灯亮度
  - switch_upper  — 上灯开关
  - mode          — 模式
```

APP 和设备之间通过 MQTT 传输物模型数据，格式是 JSON。

### 4.2 MQTT 主题（Topic）规则

```
设备上报（设备 → 平台）：
  /{productId}/{deviceNum}/property/post     ← 属性上报
  /{productId}/{deviceNum}/event/post        ← 事件上报
  /{productId}/{deviceNum}/info/post         ← 设备信息（在线状态）

平台下发（平台 → 设备）：
  /{productId}/{deviceNum}/function/get      ← 功能指令下发
```

APP 订阅设备的上报主题，就能实时收到设备状态变化。
APP 发布到功能主题，就能控制设备。

### 4.3 设备状态

| 值 | 状态  | 说明            |
| - | --- | ------------- |
| 1 | 未激活 | 刚创建，还没连过 MQTT |
| 2 | 禁用  | 管理员禁用         |
| 3 | 在线  | MQTT 已连接      |
| 4 | 离线  | MQTT 已断开      |

***

## 5. 设备详情页 — 理解 MQTT 实时通信

设备详情页是理解整个 APP 工作原理的关键，建议重点阅读。

### 5.1 页面生命周期

```
onLoad(options)
  │
  ├─ this.deviceId = options.deviceId  ← 获取传入的设备ID
  ├─ this.getDeviceInfo()               ← HTTP 请求获取设备详情
  └─ this.connectMqtt()                 ← 连接 MQTT 并订阅

onShow()
  └─ 页面显示时的逻辑

onUnload()
  └─ this.mqttUnSubscribe(device)       ← 取消订阅
```

### 5.2 MQTT 消息回调流程

```javascript
mqttCallback(topic, message) {
    // 1. 解析主题，判断是哪种消息
    // 2. 解析 message JSON
    // 3. 更新页面数据（物模型值）
    // 4. 触发 UI 刷新
}
```

### 5.3 控制设备的流程

```
用户点击按钮（比如开灯）
  ↓
APP 组装指令（物模型 identifier + value）
  ↓
APP 发布 MQTT 消息到 function/get 主题
  ↓
MQTT Broker 转发给设备
  ↓
设备执行指令
  ↓
设备上报新状态到 property/post 主题
  ↓
APP 收到上报，更新 UI
```

***

## 6. OTA 升级完整链路

### 6.1 OTA 是什么

OTA = Over-The-Air，空中升级。就是设备通过 WiFi 从服务器下载新固件，然后自动更新。

### 6.2 三方角色

```
APP (手机)        后端 (SpringBoot)        MCU (设备)
   │                   │                     │
   │  1. 查询固件版本   │                     │
   │──────────────────►│                     │
   │  2. 返回最新版本   │                     │
   │◄──────────────────│                     │
   │                   │                     │
   │  3. 下发升级指令   │                     │
   │──────────────────►│  4. MQTT 转发升级指令 │
   │                   │────────────────────►│
   │                   │                     │
   │                   │  5. 设备下载固件     │
   │                   │◄────────────────────│
   │                   │  (HTTP 下载固件文件) │
   │                   │                     │
   │                   │  6. 升级进度上报     │
   │◄───────────────────────────────────────│
   │                   │                     │
```

### 6.3 APP 端 OTA 相关代码

#### 6.3.1 固件版本查询 API

**文件**：`apis/modules/device.js`

```javascript
// 查询设备最新固件版本
getLatestFirmware(deviceId) {
    return http.get('/iot/firmware/getLatest/' + deviceId)
}
```

#### 6.3.2 设备详情页显示固件版本

**文件**：`pagesA/home/device/index.vue`

在设备信息区域显示当前固件版本号。

#### 6.3.3 下发升级指令

通过 MQTT 发布到功能主题，触发设备升级。物模型标识符通常是 `ota_upgrade` 或类似名称。

### 6.4 后端 OTA 相关模块

后端应该有以下模块（需要在 SpringBoot 代码中确认）：

| 模块     | 功能               |
| ------ | ---------------- |
| 固件管理   | 上传固件文件、版本管理、关联产品 |
| OTA 升级 | 下发升级指令、升级进度上报    |
| 文件服务   | 提供固件文件下载         |

固件表通常叫 `iot_firmware` 或类似名称。

### 6.5 MCU 端 OTA 相关代码

ESP32-C3 的 OTA 通常使用 ESP-IDF 的 `esp_https_ota` 组件：

```
设备收到升级指令（固件URL）
  → esp_https_ota_begin()     开始 OTA
  → esp_https_ota_perform()   持续下载固件
  → esp_https_ota_finish()    完成，切换启动分区
  → 重启设备
  → 新固件启动
```

升级过程中设备通过 MQTT 上报进度（百分比）。

### 6.6 OTA 物模型设计（推测）

| 类型 | identifier         | 数据类型   | 方向 | 说明          |
| -- | ------------------ | ------ | -- | ----------- |
| 属性 | `firmware_version` | string | 上报 | 当前固件版本      |
| 功能 | `ota_upgrade`      | string | 下发 | 升级指令（固件URL） |
| 属性 | `ota_progress`     | int    | 上报 | 升级进度 0-100  |
| 事件 | `ota_result`       | int    | 上报 | 升级结果（成功/失败） |

***

## 7. OTA 调试思路

### 7.1 调试顺序（从下往上）

建议按照 MCU → 后端 → APP 的顺序调试，这样每一层出问题都能快速定位。

```
Step 1: MCU 端 OTA 功能验证
  ├── 用 Postman/curl 直接给设备发升级指令，看能不能升级
  └── 验证：升级进度上报、升级成功/失败

Step 2: 后端 OTA 功能验证
  ├── 上传固件到后端
  ├── 用后端 API 触发升级
  └── 验证：MQTT 转发、固件下载、进度上报

Step 3: APP 端 OTA 功能联调
  ├── APP 显示固件版本
  ├── APP 触发升级
  └── APP 显示升级进度
```

### 7.2 MCU 端调试要点

1. **先确认 HTTP OTA 能工作**

   - 用 `esp_https_ota` 示例先跑通

   - 固件放在一个可访问的 HTTP 服务器上测试

2. **再集成到 MQTT**

   - 收到升级指令后启动 OTA

   - 升级过程中上报进度

3. **关键日志点**

   - 收到升级指令

   - 开始下载固件

   - 下载进度（每 5% 或 10% 上报一次）

   - 下载完成

   - 校验固件

   - 重启

   - 新固件启动成功（上报新版本号）

### 7.3 后端调试要点

1. **固件上传接口**

   - 能不能上传固件文件

   - 固件文件存在哪里（本地磁盘 / OSS / MinIO）

   - 下载 URL 怎么生成

2. **升级指令下发**

   - 后端 API 触发升级

   - MQTT 消息能不能发到设备

   - 设备能不能收到

3. **升级进度存储**

   - 设备上报的进度存在哪里

   - APP 怎么获取进度（MQTT 实时推送 / HTTP 查询）

### 7.4 APP 端调试要点

1. **固件版本显示**

   - 调用 `getLatestFirmware` 接口

   - 对比当前版本和最新版本

2. **升级按钮交互**

   - 点击升级 → 确认弹窗 → 下发指令

   - 显示升级进度条

   - 升级成功/失败提示

3. **MQTT 进度接收**

   - 订阅升级进度主题

   - 实时更新进度条 UI

### 7.5 常见问题排查

| 问题        | 可能原因                 | 排查方法                         |
| --------- | -------------------- | ---------------------------- |
| 设备收不到升级指令 | MQTT 订阅失败 / 主题不匹配    | 用 MQTT 工具（MQTTX）订阅同一主题看能不能收到 |
| 固件下载失败    | URL 错误 / 网络不通 / 证书问题 | 在设备上打印错误日志，用 curl 测试 URL     |
| 升级后设备启动失败 | 固件不兼容 / 校验失败         | 看串口日志，确认固件版本和分区表             |
| APP 收不到进度 | 主题不匹配 / 数据格式不对       | 用 MQTTX 订阅看设备上报的原始数据         |
| 升级进度不动    | 下载卡住 / 上报逻辑有问题       | 看设备串口日志的下载进度                 |

***

## 8. 常见问题与技巧

### 8.1 怎么快速找到某个功能的代码？

1. **从 API 入手**：先在 `apis/modules/` 里找对应的接口，然后全局搜索调用位置
2. **从页面入手**：在 `pages.json` 里找页面路径，然后看页面代码
3. **全局搜索**：用关键词搜索，比如搜 "firmware"、"upgrade"、"ota"

### 8.2 uView UI 怎么用？

uView 是这个项目的 UI 组件库，用法和 Element UI 类似：

```html
<!-- 按钮 -->
<u-button type="primary" @click="handleClick">确定</u-button>

<!-- 表单 -->
<u-form :model="form" :rules="rules">
    <u-form-item label="名称" prop="name">
        <u-input v-model="form.name" />
    </u-form-item>
</u-form>

<!-- 弹窗 -->
<u-modal v-model="show" title="提示" content="确定要升级吗？"></u-modal>
```

查文档：<https://www.uviewui.com/>

### 8.3 Vuex 怎么用？

这个项目用了 uView 提供的简化版 Vuex 用法：

```javascript
// 存
this.$u.vuex('vuex_token', 'xxx')

// 取
this.$u.vuex('vuex_token')

// 存对象
this.$u.vuex('profile', { userId: 1, userName: 'test' })

// 取对象的属性
this.$u.vuex('profile.userId')
```

本质上还是 Vuex，只是封装了一层便捷方法。

### 8.4 条件编译怎么理解？

uni-app 支持多端编译，用条件编译区分平台：

```javascript
// #ifdef MP-WEIXIN
    // 只有微信小程序才执行这里的代码
    wx.getWifiList()
// #endif

// #ifndef H5
    // 除了 H5 都执行
    uni.getUpdateManager()
// #endif

// #ifdef APP-PLUS
    // 只有 App 端才执行
    plus.runtime.restart()
// #endif
```

### 8.5 推荐的调试工具

| 工具                  | 用途                      |
| ------------------- | ----------------------- |
| **HBuilderX**       | uni-app 官方 IDE，运行/调试/打包 |
| **MQTTX**           | MQTT 客户端调试工具，订阅/发布消息    |
| **Chrome DevTools** | H5 端调试（网络面板看 HTTP 请求）   |
| **微信开发者工具**         | 小程序端调试                  |
| **串口助手**            | MCU 端调试（看 ESP32 日志）     |

***

## 总结：你的学习路径

```
Week 1: 基础过渡
  ├── 熟悉 uni-app 核心 API（navigateTo/request/storage）
  ├── 看懂 pages.json 路由配置
  ├── 跑通项目，在 H5 或小程序里登录、看设备列表
  └── 理解 uView UI 组件的基本用法

Week 2: 深入核心
  ├── 读懂设备详情页的 MQTT 通信逻辑
  ├── 理解物模型概念（属性/功能/事件）
  ├── 搞懂 API 封装层和拦截器
  └── 理解 Vuex 状态管理

Week 3: OTA 专项
  ├── 找到后端 OTA 相关代码（固件管理、升级接口）
  ├── 找到 MCU 端 OTA 实现（esp_https_ota）
  ├── 找到 APP 端 OTA UI 和逻辑
  └── 三方联调 OTA 功能
```

***

> 文档生成日期：2026-09-08
> 项目：wumei-smart-app / FastBee IoT Platform

