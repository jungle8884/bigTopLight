# XS-XLD5 V3.0 大路灯控制板 — 嵌入式开发需求分析

> 目标平台：ESP32-C3  
> 规格书版本：V3.0（2026-04-16）  
> 供应商：XXXX科技有限公司  
> 委托方：XXXX科技有限公司  

---

## 1. 项目概述

本项目为 24V 供电的大路灯控制板，核心功能是通过 PWM 驱动上下两路灯板，实现开关、无级调光、场景模式切换、不断电记忆等功能。控制板带 5 个机械按键和对应的指示灯，面向消费级照明场景。

**关键技术指标：**

| 参数 | 值 |
|------|-----|
| 外部输入电源 | DC 24V / 4A |
| 上灯最大功率 | 40W |
| 下灯最大功率 | 60W |
| PWM 输出路数 | 2 路（上灯、下灯各 1 路） |
| 按键数量 | 5 个 |
| 指示灯数量 | 5 个 |
| 记忆功能 | 非易失存储（亮度级别、开关状态） |

---

## 2. 需求拆解

### 2.1 模块特性（规格书原文提炼）

1. **高精 PWM 输出，平滑调光** — 需要 ≥10bit 分辨率的 PWM，保证亮度变化的连续性，无可见阶梯
2. **开关时灯光缓起缓灭，温和不刺眼** — 开灯时亮度从 0 渐变到目标值，关灯时从当前值渐变到 0，过渡时间约 1–2s
3. **多使用场景可随意切换** — 至少 3 种模式：正常模式、阅读模式、夜灯模式
4. **可自定义灯光亮度** — 长按无级调节 + 双击极值切换
5. **不断电记忆功能** — 掉电后亮度设置不丢失，重新上电后恢复

### 2.2 硬件接口

| 接口号 | 名称 | 功能 | 备注 |
|--------|------|------|------|
| 1 | 电源输入接口 | DC24V 输入 | 注意正负极 |
| 2 | LED 灯板正极接口 | LED 灯板正极 | 注意灯板极性 |
| 3 | LED 灯板负极接口 | LED 灯板负极 | 注意灯板极性 |
| 4 | 功能选择区 | 选择使用场景 | 5 个按键 |

硬件层面，MCU 的 PWM 信号经过 MOSFET 驱动 24V LED 负载。MCU GPIO 本身不直接驱动大电流，PWM GPIO → 栅极驱动电阻 → N-MOSFET 栅极 → MOSFET 漏极接 LED 负载负极（低边驱动）或高边驱动方案。

### 2.3 按键功能矩阵

规格书定义了 5 个按键，每个按键支持短按、长按、双击三种操作：

| 按键 | 功能 | 短按 | 长按 | 双击 | 指示灯 |
|------|------|------|------|------|--------|
| **开关键** | 上下灯总开关 | 上下灯全开/全关切换 | — | — | 主灯亮时点亮，熄灭时熄灭 |
| **上灯键** | 上灯开关+调光 | 上灯开/关切换 | 亮度无级调节（递增/递减） | 最暗↔最亮切换 | 上灯亮时点亮，熄灭时熄灭 |
| **下灯键** | 下灯开关+调光 | 下灯开/关切换 | 亮度无级调节（递增/递减） | 最暗↔最亮切换 | 下灯亮时点亮，熄灭时熄灭 |
| **阅读键** | 阅读模式 | 进入阅读模式（上下灯均 100%） | — | — | 阅读模式开启时点亮 |
| **夜灯键** | 夜灯模式 | 进入夜灯模式（上灯 5%、下灯灭） | — | — | 夜灯模式开启时点亮 |

### 2.4 各按键行为详细规则

#### 2.4.1 开关键

- **上电行为**：上电时所有指示灯亮 1 秒后熄灭，系统初始状态为全关（上下灯均灭）
- **短按**：上下灯均灭时 → 开上下灯（恢复记忆亮度）；上下灯均亮时 → 关上下灯
- 循环切换，无长按和双击功能
- 指示灯跟随主灯状态

#### 2.4.2 上灯键 / 下灯键（行为一致，对象不同）

- **短按（灯灭时）**：开灯，恢复记忆亮度
- **短按（灯亮时）**：关灯
- **长按（灯亮时）**：进入亮度无级调节，亮度持续变化（到达上限后反向递减，到达下限后反向递增，形成往复循环）。松开按键时停在当前亮度并保存
- **双击（灯亮时）**：第一次双击 → 亮度调到最暗；第二次双击 → 亮度调到最亮；循环
- **前提条件**：阅读模式/夜灯模式下，按下上灯键或下灯键会先退出该模式（视为"其他键"），不执行开/关动作

#### 2.4.3 阅读键

- **触发条件**：开灯状态下（主灯亮）
- **进入阅读模式**：上下灯均以 100% 亮度点亮
- **重复按下**：若当前已处于阅读模式，再次按下无响应
- **退出条件**：阅读模式期间，任意其他按键按下 → 退出阅读模式，恢复进入前的亮度和开关状态
- 指示灯：阅读模式开启时点亮，关闭时熄灭

#### 2.4.4 夜灯键

- **触发条件**：开灯状态下（主灯亮）
- **进入夜灯模式**：上灯以 5% 亮度点亮，下灯熄灭
- **重复按下**：若当前已处于夜灯模式，再次按下无响应
- **30 分钟计时**：进入夜灯模式后开始 30 分钟倒计时，到时自动关灯（系统进入全关状态）
- **提前退出**：若 30 分钟内退出夜灯模式，计时归 0；再次进入重新开始 30 分钟计时
- **退出条件**：夜灯模式期间，任意其他按键按下 → 退出夜灯模式，恢复进入前的亮度和开关状态
- 指示灯：夜灯模式开启时点亮，关闭时熄灭

### 2.5 模式互斥关系

```
正常模式 ←──── 任意其他键 ────→ 阅读模式
    ↑                              
    └──── 任意其他键 ────→ 夜灯模式
                              │
                         30min超时
                              │
                              ↓
                           全关状态
```

阅读模式和夜灯模式互斥：进入其中一个模式后，另一个键也属于"其他键"，会退出当前模式。但由于按下夜灯键/阅读键本身就是"其他键"，这里需要明确：**在阅读模式下按夜灯键 → 先退出阅读模式，然后进入夜灯模式**（一次按键完成两个动作：退出旧模式 + 进入新模式）。

### 2.6 记忆功能定义

| 存储内容 | 时机 | 恢复时机 |
|----------|------|----------|
| 上灯亮度值 | 调光时、关灯时 | 开灯时恢复 |
| 下灯亮度值 | 调光时、关灯时 | 开灯时恢复 |
| 上灯开/关状态 | 关键关闭时 | — |
| 下灯开/关状态 | 关键关闭时 | — |

> **设计决策点**：上电后是否自动恢复到关灯前的开关状态？  
> 规格书说"上电时所有指示灯亮 1 秒后熄灭"，暗示上电后系统处于全关状态，需要按开关键才会开灯。因此记忆功能仅保存亮度级别，上电后初始状态为全关。

---

## 3. ESP32-C3 外设映射

### 3.1 芯片资源概览

ESP32-C3（RISC-V 单核 32位，160MHz）关键外设：

| 外设 | 数量 | 本项目用途 |
|------|------|------------|
| LEDC（LED PWM 控制器） | 6 通道 | 上灯 PWM + 下灯 PWM（2 通道） |
| GPIO | 最多 22 个 | 按键输入 + PWM 输出 + 指示灯输出 |
| 通用定时器 | 2 组 | 夜灯 30min 计时 + 按键扫描/调光计时 |
| SPI Flash（内嵌） | 通常 4MB | NVS 非易失存储 |
| RTC 定时器 | 1 组 | 低功耗定时（备用） |

### 3.2 GPIO 分配建议

ESP32-C3 QFN32 封装有 22 个 GPIO（GPIO0–GPIO21），其中部分有特殊用途需避开：

| 功能 | GPIO 建议引脚 | 说明 |
|------|---------------|------|
| 上灯 PWM 输出 | GPIO2 | LEDC 通道 0，驱动 MOSFET 栅极 |
| 下灯 PWM 输出 | GPIO3 | LEDC 通道 1，驱动 MOSFET 栅极 |
| 开关键 输入 | GPIO4 | 输入 + 下拉，外部上拉 |
| 上灯键 输入 | GPIO5 | 输入 + 下拉，外部上拉 |
| 下灯键 输入 | GPIO6 | 输入 + 下拉，外部上拉 |
| 阅读键 输入 | GPIO7 | 输入 + 下拉，外部上拉 |
| 夜灯键 输入 | GPIO8 | 输入 + 下拉，外部上拉 |
| 开关键指示灯 | GPIO9 | GPIO 输出，驱动 LED |
| 上灯键指示灯 | GPIO10 | GPIO 输出，驱动 LED |
| 下灯键指示灯 | GPIO18 | GPIO 输出，驱动 LED |
| 阅读键指示灯 | GPIO19 | GPIO 输出，驱动 LED |
| 夜灯键指示灯 | GPIO20 | GPIO 输出，驱动 LED |

> **注意避开**：GPIO11–GPIO17 默认连接 SPI Flash（JTAG/SPI），不可用作普通 GPIO；GPIO0 为 Boot 控制引脚，上电时需为低电平进入下载模式，慎用。GPIO18–GPIO20 在某些封装上可用，需确认具体型号 datasheet。 

### 3.3 LEDC PWM 配置

```
参数建议：
  频率：1000 Hz（无可见闪烁，>200Hz 即可，1kHz 更安全）
  占空比分辨率：10 bit（1024 级，0–1023 对应 0%–100%）
  通道数：2（通道0=上灯，通道1=下灯）
  
LEDC 优势：
  - 硬件渐变（ledc_set_fade_with_time）可直接实现缓起缓灭
  - 无需 CPU 干预即可完成亮度过渡
  - 渐变完成后产生中断通知
```

### 3.4 NVS 存储设计

```
命名空间 (namespace): "lamp_ctrl"

键值表：
  brightness_upper  (uint8_t)  上灯亮度 0–100
  brightness_lower  (uint8_t)  下灯亮度 0–100
  state_upper       (uint8_t)  上灯开关 0/1
  state_lower       (uint8_t)  下灯开关 0/1

写入时机：
  - 长按调光松开时（亮度变化）
  - 双击切换亮度时
  - 开关键关闭时（保存当前开关状态+亮度）
  - 退出阅读/夜灯模式时（如亮度有变化）

读取时机：
  - 上电初始化后从 NVS 加载默认参数
  - 开关键开灯时恢复亮度
```

---

## 4. 软件架构设计

### 4.1 整体架构

采用 ESP-IDF + FreeRTOS，分层架构：

```
┌─────────────────────────────────────────────────┐
│                   应用层 (app_main)              │
│         状态机引擎 + 模式管理 + 事件分发           │
├─────────────┬───────────┬───────────────────────┤
│  按键模块    │  PWM模块    │   指示灯模块           │
│  button.c   │  pwm_drv.c │  indicator.c          │
├─────────────┼───────────┼───────────────────────┤
│  存储模块    │  定时器模块 │   MQTT客户端模块       │
│  storage.c  │  timer.c  │  mqtt_client.c        │
├─────────────┴───────────┴───────────────────────┤
│              ESP-IDF 驱动层 (HAL)                 │
│  ledc / gpio / nvs_flash / esp_timer / freertos  │
│  esp_wifi / esp-mqtt / cJSON                     │
├─────────────────────────────────────────────────┤
│              ESP32-C3 硬件                        │
└─────────────────────────────────────────────────┘
```

### 4.2 FreeRTOS 任务划分

| 任务名 | 优先级 | 职责 |
|--------|--------|------|
| `button_task` | 中（5） | 轮询 GPIO 状态（10ms），消抖，识别短按/长按/双击，发送事件到队列 |
| `lamp_task` | 高（7） | 主状态机循环，处理按键/远程/定时事件，控制模式切换、PWM 输出、指示灯、MQTT 上报 |

> **ESP-IDF 内部任务**：esp-mqtt 客户端和 esp_wifi 各有内部任务，不需要手动创建。渐变调光使用 `esp_timer` 回调实现，无需独立任务。

**核心数据流：**

```
GPIO轮询 → button_task消抖识别 ──┐
MQTT function/get ──────────────┼──→ 事件队列 → lamp_task状态机 → PWM/指示灯/MQTT/存储
esp_timer 回调 ─────────────────┘
```

按键、远程命令、定时器事件用统一的 `system_msg_t` 结构通过 FreeRTOS 队列传递：

```c
typedef struct {
    msg_type_t type;        // MSG_BUTTON / MSG_COMMAND / MSG_TIMER
    button_id_t btn_id;
    button_event_t btn_evt;
    command_id_t cmd_id;
    lamp_id_t lamp_id;
    int32_t value;          // 亮度值 / 模式枚举 / 开关 0|1
    timer_id_t timer_id;
} system_msg_t;
```

---

## 5. 状态机设计

### 5.1 系统级状态

```
SYSTEM_OFF  ←── 开关键短按(灯亮时) ──  SYSTEM_ON
    │                                        │
    │                                    ┌───┴───┐
    │                               NORMAL  READING  NIGHT
    │                                 ↑       ↑       ↑
    └── 夜灯30min超时 ────────────────┘   └───────┘
                                    (任意其他键退出)
```

### 5.2 详细状态转移表

| 当前状态 | 触发事件 | 目标状态 | 动作 |
|----------|----------|----------|------|
| SYSTEM_OFF | 开关键短按 | NORMAL | 恢复记忆亮度，PWM 缓起，开关键指示灯亮 |
| SYSTEM_OFF | 其他键 | SYSTEM_OFF | 无响应 |
| NORMAL | 开关键短按 | SYSTEM_OFF | PWM 缓灭，保存状态，指示灯灭 |
| NORMAL | 上灯键短按 | NORMAL | 切换上灯开/关 |
| NORMAL | 下灯键短按 | NORMAL | 切换下灯开/关 |
| NORMAL | 上灯键长按(灯亮时) | NORMAL | 进入调光循环 |
| NORMAL | 下灯键长按(灯亮时) | NORMAL | 进入调光循环 |
| NORMAL | 上灯键双击(灯亮时) | NORMAL | 切换最暗/最亮 |
| NORMAL | 下灯键双击(灯亮时) | NORMAL | 切换最暗/最亮 |
| NORMAL | 阅读键短按 | READING | 上下灯均 100%，阅读指示灯亮 |
| NORMAL | 夜灯键短按 | NIGHT | 上灯 5%，下灯灭，启动 30min 计时 |
| READING | 阅读键短按 | READING | 无响应 |
| READING | 其他任意键 | NORMAL | 恢复进入前亮度，阅读指示灯灭 |
| NIGHT | 夜灯键短按 | NIGHT | 无响应 |
| NIGHT | 阅读键短按 | READING | 退出夜灯（停计时），进阅读模式 |
| NIGHT | 其他任意键 | NORMAL | 恢复进入前亮度，夜灯指示灯灭，停计时 |
| NIGHT | 30min 超时 | SYSTEM_OFF | PWM 缓灭，全关，夜灯指示灯灭 |

### 5.3 长按调光子状态

长按调光是一个持续过程，不属于状态转移，而是在 NORMAL 状态下的持续动作：

```
开始长按 → 当前亮度 → 方向判定(当前<50%则递增，≥50%则递减)
  → 每 20ms 步进 1% → 到达上限(100%) → 反向递减
  → 到达下限(5%) → 反向递增 → 松开 → 保存亮度到 NVS
```

> **方向判定策略**：也可设计为首次长按总是递增，到达 100% 后自动反向。两种方案都满足"向上（向下）进行亮度无级调节"的描述。建议采用"到达极值自动反向"方案，用户只需长按不放即可在 5%–100% 之间往复扫掠。

### 5.4 双击亮度切换

```
当前亮度 ≠ 最暗 → 双击 → 亮度设为最暗(MIN_BRIGHTNESS)
当前亮度 = 最暗 → 双击 → 亮度设为最亮(100%)
```

需要记录双击前的"是否已经是最暗"状态。简化为：维护一个 toggle 变量，每次双击翻转（最暗 ↔ 最亮）。

---

## 6. 核心模块分析

### 6.1 PWM 调光模块（LEDC 驱动）

**初始化：**

```c
// LEDC 定时器配置
ledc_timer_config_t timer_cfg = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .timer_num       = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_10_BIT,   // 0–1023
    .freq_hz         = 1000,                 // 1kHz
    .clk_cfg         = LEDC_AUTO_CLK,
};

// 通道配置（上灯）
ledc_channel_config_t upper_ch = {
    .channel    = LEDC_CHANNEL_0,
    .gpio_num   = UPPER_LAMP_PWM_GPIO,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 0,
    .hpoint     = 0,
};
// 下灯同理，CHANNEL_1
```

**缓起缓灭（硬件渐变）：**

```c
// 开灯缓起：0 → target，持续 1500ms
ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, channel, 
                         brightness_to_duty(target), 1500);
ledc_fade_start(LEDC_LOW_SPEED_MODE, channel, LEDC_FADE_NO_WAIT);

// 关灯缓灭：current → 0，持续 1000ms
ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, channel, 0, 1000);
ledc_fade_start(LEDC_LOW_SPEED_MODE, channel, LEDC_FADE_NO_WAIT);
```

**亮度百分比到占空比转换：**

```
duty = brightness_pct * 1023 / 100
最暗(MIN_BRIGHTNESS) = 5% → duty = 51
最亮(MAX_BRIGHTNESS) = 100% → duty = 1023
```

### 6.2 按键检测模块

ESP32-C3 按键检测需要区分三种操作，核心是消抖 + 时序判定。

**消抖方案：** GPIO 边沿中断触发后，启动一个 20ms 软件定时器，定时器回调时再次读取 GPIO 电平确认。使用 FreeRTOS 任务轮询方式也可。

**三种操作的时序定义：**

```
短按：按下后在 LONG_PRESS_THRESHOLD(约800ms) 内释放
长按：按下时间 ≥ LONG_PRESS_THRESHOLD
  - 长按开始：触发 LONG_PRESS_START 事件
  - 长按释放：触发 LONG_PRESS_RELEASE 事件
双击：第一次短按释放后，在 DOUBLE_CLICK_WINDOW(约300ms) 内再次按下
```

**状态机（单个按键）：**

```
IDLE → 按下 → DEBOUNCE_PRESS(20ms消抖)
  → 确认按下 → PRESSED
    → 800ms内释放 → WAIT_DOUBLE(等300ms看是否双击)
      → 300ms内再次按下 → DEBOUNCE_PRESS → 确认 → 发送DOUBLE_CLICK → IDLE
      → 300ms超时 → 发送SHORT_PRESS → IDLE
    → 800ms未释放 → 发送LONG_PRESS_START → LONG_HELD
      → 释放 → 发送LONG_PRESS_RELEASE → IDLE
```

**按键事件队列：** 每个按键独立运行上述状态机，检测到事件后通过 `xQueueSend` 发送到主任务队列。

### 6.3 模式管理模块

负责模式切换时的亮度保存/恢复逻辑：

```
进入阅读模式：
  1. 保存当前上下灯亮度 + 开关状态（快照）
  2. 上灯 PWM → 100%（缓起）
  3. 下灯 PWM → 100%（缓起）
  4. 开关键指示灯保持，上/下灯指示灯保持，阅读指示灯亮

退出阅读模式（任意其他键）：
  1. 恢复快照中的亮度 + 开关状态
  2. 若快照中灯是灭的 → PWM 缓灭到 0
  3. 若快照中灯是亮的 → PWM 缓变到保存的亮度
  4. 阅读指示灯灭

进入夜灯模式：
  1. 保存当前上下灯亮度 + 开关状态（快照）
  2. 上灯 PWM → 5%（缓变）
  3. 下灯 PWM → 0（缓灭）
  4. 夜灯指示灯亮，启动 30min 倒计时定时器

退出夜灯模式（任意其他键）：
  1. 恢复快照
  2. 停止 30min 计时
  3. 夜灯指示灯灭

夜灯 30min 超时：
  1. 全部 PWM → 0（缓灭）
  2. 系统进入 SYSTEM_OFF
  3. 所有指示灯灭
```

### 6.4 非易失存储模块（NVS）

```c
// 初始化
nvs_flash_init();

// 读取
nvs_open("lamp_ctrl", NVS_READONLY, &handle);
nvs_get_u8(handle, "brightness_upper", &b_upper);
nvs_get_u8(handle, "brightness_lower", &b_lower);
// 首次使用（key不存在）→ 设默认值：上灯 100%，下灯 100%

// 写入
nvs_open("lamp_ctrl", NVS_READWRITE, &handle);
nvs_set_u8(handle, "brightness_upper", new_val);
nvs_commit(handle);
```

**写入策略优化：** 避免频繁写入磨损 Flash。长按调光时不实时写 NVS，仅在松开时写一次。双击和模式切换时各写一次。

### 6.5 定时器模块（夜灯 30min 计时）

使用 ESP-IDF `esp_timer` API（高分辨率软件定时器，基于硬件定时器）：

```c
// 创建一次性定时器
esp_timer_handle_t night_timer;
esp_timer_create_args_t timer_cfg = {
    .callback = night_lamp_timeout_callback,
    .name = "night_lamp",
};
esp_timer_create(&timer_cfg, &night_timer);

// 进入夜灯模式时启动 (30分钟)
esp_timer_start_once(night_timer, 30 * 60 * 1000 * 1000);  // microseconds

// 退出夜灯模式时停止
esp_timer_stop(night_timer);

// 重新进入时再启动 (会覆盖之前的定时)
esp_timer_start_once(night_timer, 30 * 60 * 1000 * 1000);
```

回调函数中发送一个 `MSG_TIMER` 消息到主队列，由状态机处理超时关灯。

### 6.6 指示灯模块

5 个指示灯由 GPIO 直接驱动（通过限流电阻接 LED）。指示灯状态完全跟随系统状态，无需独立逻辑：

```c
// 上电全亮 1 秒
for (int i = 0; i < 5; i++) gpio_set_level(led_gpios[i], 1);
vTaskDelay(pdMS_TO_TICKS(1000));
for (int i = 0; i < 5; i++) gpio_set_level(led_gpios[i], 0);

// 状态更新函数（每次状态机切换时调用）
void update_indicators(system_state_t state, lamp_status_t *status) {
    gpio_set_level(LED_POWER,    state != SYSTEM_OFF);
    gpio_set_level(LED_UPPER,    status->upper_on);
    gpio_set_level(LED_LOWER,    status->lower_on);
    gpio_set_level(LED_READING,  state == READING);
    gpio_set_level(LED_NIGHT,    state == NIGHT);
}
```

### 6.7 MQTT 客户端模块（FastBee 平台对接）

设备通过 MQTT 协议对接 FastBee 物联网平台，实现远程控制和状态上报。

**MQTT 连接配置（简单认证模式）：**

```
Broker:    mqtt://{FB_MQTT_HOST}:{FB_MQTT_PORT}  (默认 1883)
ClientId:  S&{deviceNum}&{productId}&{userId}     (S=简单认证)
Username:  平台认证账号
Password:  平台认证密码 (明文)
KeepAlive: 120s
LWT:       info/post 主题, {"status":4} (离线)
```

**FastBee 主题结构 — 前缀 `/{productId}/{deviceNum}`：**

| 主题 | 方向 | 用途 |
|------|------|------|
| `.../property/post` | 发布 | 属性数据上报 (bright_upper, bright_lower) |
| `.../function/get` | 订阅 | 接收平台下发的功能指令 |
| `.../function/post` | 发布 | 功能状态回复 (power, mode, switch_upper, switch_lower) |
| `.../info/post` | 发布 | 设备信息 (rssi, 固件版本, 在线状态) |

**物模型映射：**

| 标识符 | 物模型类别 | 数据类型 | 数据流 |
|--------|-----------|----------|--------|
| `bright_upper` | 属性 | 整数(0-100) | property/post 上报 |
| `bright_lower` | 属性 | 整数(0-100) | property/post 上报 |
| `power` | 功能 | 布尔(0/1) | function/get 接收, function/post 回复 |
| `switch_upper` | 功能 | 布尔(0/1) | function/get 接收, function/post 回复 |
| `switch_lower` | 功能 | 布尔(0/1) | function/get 接收, function/post 回复 |
| `mode` | 功能 | 字符串 | function/get 接收, function/post 回复 |

**消息格式（物模型 JSON 数组）：**

```
属性上报 (property/post):
  [{"id":"bright_upper","value":"80","remark":""},
   {"id":"bright_lower","value":"0","remark":""}]

功能指令接收 (function/get):
  [{"id":"power","value":"1"},
   {"id":"mode","value":"reading"}]

功能状态回复 (function/post):
  [{"id":"power","value":"1","remark":""},
   {"id":"mode","value":"normal","remark":""},
   {"id":"switch_upper","value":"1","remark":""},
   {"id":"switch_lower","value":"0","remark":""}]

设备信息 (info/post):
  {"rssi":-55,"firmwareVersion":"1.0","status":3,"userId":"1",
   "summary":{"name":"XS-XLD5","chip":"ESP32-C3","version":"1.0"}}
```

**数据流：**

```
FastBee 平台 → MQTT function/get → parse_function_command() → state_machine_send_event()
state_machine → mqtt_publish_state() → MQTT property/post  (属性: 亮度)
                                   → MQTT function/post  (功能: 开关/模式)
Wi-Fi 连接成功 → mqtt_app_start() → MQTT 连接 → 订阅 function/get + 发布 info/post
```

---

## 7. 技术难点与解决方案

### 7.1 缓起缓灭与调光的衔接

**问题**：开灯时缓起到目标亮度（1.5s），长按调光时实时改变亮度，两者需要平滑衔接。

**方案**：统一使用 LEDC 硬件渐变 API。缓起/缓灭用 `ledc_set_fade_with_time`，长按调光用快速小步渐变（每 20ms 调用 `ledc_set_duty_and_update`），双击切极值用 `ledc_set_fade_with_time` 快速渐变（200ms）。

### 7.2 双击与长按的时序冲突

**问题**：双击需要等待 300ms 判断是否有第二次按下，但长按 800ms 后才触发。如果用户按下后不放，如何区分"准备双击的第一次短按"和"长按"？

**方案**：按下后启动 800ms 计时器。在 800ms 内释放 → 进入双击等待窗口（300ms）；800ms 超时未释放 → 确认为长按。这是标准按键处理方案，时序图如下：

```
按下 ──→ 等待 ──→ 800ms内释放? ──是──→ 等双击(300ms) ──→ 有二次按下: 双击
  │                    │                           └─→ 无: 短按
  │                    否
  └──→ 长按开始 ──→ 释放: 长按结束
```

### 7.3 阅读模式/夜灯模式退出后的亮度恢复

**问题**：进入阅读/夜灯模式时修改了亮度，退出后需要恢复进入前的亮度。但进入前的亮度可能是一个正在渐变中的中间值。

**方案**：进入特殊模式时保存当前的逻辑亮度值（百分比，非 PWM duty），退出时恢复。进入/退出都用渐变过渡。保存的快照包含：上灯亮度%、下灯亮度%、上灯开关、下灯开关。

### 7.4 夜灯模式与阅读模式之间的切换

**问题**：在阅读模式下按夜灯键，规格书说"其他键退出"，但夜灯键本身不是"无操作"——它应该退出阅读模式并进入夜灯模式。

**方案**：状态机中 NIGHT 和 READING 的触发键不算作"其他键"。即：
- READING 状态 + 夜灯键 → 退出 READING + 进入 NIGHT（一次按键完成模式切换）
- NIGHT 状态 + 阅读键 → 退出 NIGHT + 进入 READING
- 其他键（开关键、上灯键、下灯键）→ 仅退出当前模式，回到 NORMAL

### 7.5 Flash 写入磨损

**问题**：长按调光时亮度连续变化，如果每次步进都写 NVS 会快速磨损 Flash。

**方案**：
- 长按调光过程中仅更新内存中的亮度变量，不写 NVS
- 松开按键时写一次 NVS
- 双击切换时写一次
- 开关键关闭时写一次
- 模式退出时如亮度有变化写一次
- 估算：正常使用每天写入 < 100 次，Flash 寿命 10 万次 → 寿命 > 3 年

---

## 8. 实现路线图

### Phase 1：基础驱动搭建

- [x] LEDC 初始化，2 通道 PWM 输出验证
- [x] GPIO 按键输入 + 轮询扫描
- [x] NVS 初始化 + 读写测试
- [x] 指示灯 GPIO 输出验证
- [x] 上电全亮 1 秒逻辑

### Phase 2：按键检测

- [x] 消抖逻辑实现
- [x] 短按/长按/双击状态机
- [x] 事件队列发送
- [x] 单按键全部三种事件验证

### Phase 3：状态机核心

- [x] 系统状态枚举定义
- [x] 状态转移表实现
- [x] 开关键短按开/关灯
- [x] 上灯/下灯键短按开/关
- [x] 上电初始状态

### Phase 4：调光功能

- [x] 长按调光循环（递增/递减/反向）
- [x] 双击最暗/最亮切换
- [x] 缓起缓灭
- [x] NVS 亮度保存/恢复

### Phase 5：模式管理

- [x] 阅读模式进入/退出
- [x] 夜灯模式进入/退出
- [x] 30 分钟定时器（esp_timer）
- [x] 模式间亮度快照恢复
- [x] 模式互斥逻辑

### Phase 6：FastBee MQTT 集成

- [x] Wi-Fi 连接（STA 模式，事件驱动）
- [x] MQTT 客户端初始化（ESP-IDF esp-mqtt）
- [x] FastBee 简单认证（ClientId + 用户名 + 密码）
- [x] LWT 遗嘱消息（info/post, {"status":4}）
- [x] 订阅 function/get 主题
- [x] 功能指令解析（JSON 数组格式）
- [x] 属性上报（property/post: bright_upper, bright_lower）
- [x] 功能状态回复（function/post: power, mode, switch_upper, switch_lower）
- [x] 设备信息上报（info/post: RSSI, 固件版本, status=3）
- [ ] 平台物模型配置（属性 + 功能定义）
- [ ] 端到端联调测试

### Phase 7：集成测试

- [ ] 全功能联调
- [ ] 边界场景测试（上电→开灯→调光→进入阅读→退出→进入夜灯→超时关灯）
- [ ] 掉电记忆测试
- [ ] 多次按键压力测试
- [ ] MQTT 断线重连测试
- [ ] 平台远程控制验证

---

## 9. 关键参数定义

| 参数名 | 实际宏 | 值 | 说明 |
|--------|--------|-----|------|
| PWM 频率 | `PWM_FREQUENCY_HZ` | 1000 Hz | LED 调光频率 |
| PWM 分辨率 | `PWM_RESOLUTION_BIT` | 10 bit | 占空比 0–1023 |
| 最暗亮度 | `MIN_BRIGHTNESS` | 5% | 与夜灯模式一致 |
| 最亮亮度 | `MAX_BRIGHTNESS` | 100% | — |
| 默认亮度 | `DEFAULT_BRIGHTNESS` | 100% | 上电默认 |
| 夜灯亮度 | `NIGHT_BRIGHTNESS` | 5% | 夜灯模式上灯亮度 |
| 缓起时间 | `FADE_IN_TIME_MS` | 1500 ms | 开灯缓起 |
| 缓灭时间 | `FADE_OUT_TIME_MS` | 1000 ms | 关灯缓灭 |
| 模式过渡 | `MODE_FADE_TIME_MS` | 500 ms | 模式切换亮度过渡 |
| 双击过渡 | `DOUBLE_CLICK_FADE_MS` | 200 ms | 双击极值切换 |
| 调光步进间隔 | `DIM_STEP_INTERVAL_MS` | 20 ms | 长按调光步进 |
| 调光步进量 | `DIM_STEP_PCT` | 1% | 每步亮度变化 |
| 长按阈值 | `LONG_PRESS_THRESHOLD` | 800 ms | 长按判定阈值 |
| 双击窗口 | `DOUBLE_CLICK_WINDOW` | 300 ms | 双击间隔窗口 |
| 消抖时间 | `DEBOUNCE_MS` | 20 ms | 按键消抖 |
| 夜灯超时 | `NIGHT_LAMP_TIMEOUT_MS` | 30 min | 夜灯自动关灯 |
| 上电指示 | `POWER_ON_INDICATOR_MS` | 1000 ms | 上电全亮时间 |
| 按键轮询 | `BUTTON_POLL_MS` | 10 ms | 按键扫描间隔 |

---

## 10. 待确认事项

以下在规格书中未明确，需要与委托方（XXXX科技）确认：

1. **上电自动恢复**：掉电前如果灯是亮的，重新上电后是否需要自动恢复到亮灯状态？当前理解为上电全灭，需手动开灯。
2. **长按调光方向**：首次长按时亮度是固定从递增开始，还是根据当前亮度自动判定方向（低于 50% 递增，高于 50% 递减）？建议方案：到达极值自动反向。
3. **阅读/夜灯模式互切**：在阅读模式按夜灯键，是直接切换到夜灯模式（退出阅读+进入夜灯），还是仅退出阅读模式？当前分析为直接切换。
4. **模式退出后恢复亮度**：退出阅读/夜灯模式后，亮度恢复到进入前的值，还是保持模式中的值？当前理解为恢复进入前的值。
5. **开灯时单独控制一盏灯**：系统开灯（开关键）后上下灯均亮。此时短按上灯键关闭上灯，下灯保持亮。再按开关键，是关闭所有灯（包括已灭的上灯和亮着的下灯），还是有其他逻辑？当前理解为开关键控制上下灯总体开关，不论各自单独状态。
6. **最暗亮度定义**：双击"最暗"的亮度值是否就是夜灯模式的 5%？还是有更低的值（如 1%）？当前建议 5%。
7. **PWM 驱动方式**：硬件上是低边驱动（N-MOSFET 开关负极）还是高边驱动？影响 MOSFET 选型和 PWM 极性（正占空比 vs 负占空比）。
8. **指示灯类型**：指示灯是独立 LED 还是按键背光？是否需要 PWM 调光还是简单 GPIO 开关？
9. **功能选择区**：接口表提到"功能选择区——选择使用场景"，这是指 5 个按键本身，还是有一个独立的拨码开关/旋钮用于选择场景？规格书正文描述的是按键操作，推测就是按键区。
10. **防抖/EMC 要求**：是否有额外的 EMC 测试要求或防浪涌要求？24V 电源端是否需要 TVS/保险丝？    

---

## 11 烧录

正常运行：【烧录加上GPIO9-接地】

- EN接高电平，可以稳定输出日志

- 3.3v GND RX TX
