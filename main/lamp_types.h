/**
 * lamp_types.h — 公共类型、常量与引脚定义
 *
 * XS-XLD5 V3.0 大路灯控制板
 * MCU: ESP32-C3 | 框架: ESP-IDF + FreeRTOS
 */
#ifndef LAMP_TYPES_H
#define LAMP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ═══════════════════════════════════════════════════
 * GPIO 引脚定义
 * 注意: GPIO11~17 连接 SPI Flash, 不可用
 * ═══════════════════════════════════════════════════ */

/* 按键输入引脚 (active low, 外部上拉) */
#define GPIO_BTN_POWER      4   /* 开关键 */
#define GPIO_BTN_UPPER      5   /* 上灯键 */
#define GPIO_BTN_LOWER      6   /* 下灯键 */
#define GPIO_BTN_READING    7   /* 阅读键 */
#define GPIO_BTN_NIGHT      8   /* 夜灯键 */

/* PWM 输出引脚 (驱动 MOSFET 栅极) */
#define GPIO_PWM_UPPER      2   /* 上灯 PWM (LEDC 通道 0) */
#define GPIO_PWM_LOWER      3   /* 下灯 PWM (LEDC 通道 1) */

/* 指示灯输出引脚 (GPIO 直接驱动 LED) */
#define GPIO_LED_POWER      9   /* 开关键指示灯 */
#define GPIO_LED_UPPER      10  /* 上灯键指示灯 */
#define GPIO_LED_LOWER      18  /* 下灯键指示灯 (避开 SPI Flash 引脚) */
#define GPIO_LED_READING    19  /* 阅读键指示灯 */
#define GPIO_LED_NIGHT      20  /* 夜灯键指示灯 */

/* ═══════════════════════════════════════════════════
 * 按键枚举
 * ═══════════════════════════════════════════════════ */

typedef enum {
    BTN_POWER = 0,      /* 开关键 */
    BTN_UPPER,          /* 上灯键 */
    BTN_LOWER,          /* 下灯键 */
    BTN_READING,        /* 阅读键 */
    BTN_NIGHT,          /* 夜灯键 */
    BTN_COUNT           /* 按键总数, 用于数组索引 */
} button_id_t;

/* 按键事件类型 */
typedef enum {
    BTN_EVT_SHORT_PRESS = 0,      /* 短按: 按下后在 800ms 内释放 */
    BTN_EVT_LONG_PRESS_START,     /* 长按开始: 按下超过 800ms */
    BTN_EVT_LONG_PRESS_RELEASE,   /* 长按释放: 松开按键 */
    BTN_EVT_DOUBLE_CLICK,         /* 双击: 300ms 内二次短按 */
} button_event_t;

/* ═══════════════════════════════════════════════════
 * 灯枚举
 * ═══════════════════════════════════════════════════ */

typedef enum {
    LAMP_UPPER = 0,     /* 上灯 (最大 40W) */
    LAMP_LOWER,         /* 下灯 (最大 60W) */
    LAMP_COUNT
} lamp_id_t;

/* ═══════════════════════════════════════════════════
 * 系统状态枚举
 * ═══════════════════════════════════════════════════ */

typedef enum {
    STATE_OFF = 0,      /* 全关: 上下灯均灭 */
    STATE_NORMAL,       /* 正常模式: 独立控制上下灯 */
    STATE_READING,      /* 阅读模式: 上下灯均 100% */
    STATE_NIGHT,        /* 夜灯模式: 上灯 5%, 下灯灭, 30min 定时 */
} system_state_t;

/* 模式字符串 (MQTT 上报用) */
#define MODE_STR_OFF      "off"
#define MODE_STR_NORMAL   "normal"
#define MODE_STR_READING  "reading"
#define MODE_STR_NIGHT    "night"

/* ═══════════════════════════════════════════════════
 * 消息类型 (统一事件队列)
 * ═══════════════════════════════════════════════════ */

typedef enum {
    MSG_BUTTON = 0,     /* 按键事件 (来自 button_task) */
    MSG_COMMAND,        /* 远程命令 (来自 MQTT) */
    MSG_TIMER,          /* 定时器事件 (来自 esp_timer) */
    MSG_OTA_START,      /* OTA 升级启动 (来自 MQTT upgrade/set) */
} msg_type_t;

/* 远程命令 ID */
typedef enum {
    CMD_POWER_TOGGLE = 0,   /* 总开关切换 */
    CMD_LAMP_TOGGLE,        /* 单灯开关切换 */
    CMD_SET_BRIGHTNESS,     /* 设置亮度 */
    CMD_SET_MODE,           /* 设置工作模式 */
} command_id_t;

/* 定时器 ID */
typedef enum {
    TIMER_NIGHT_LAMP = 0,   /* 夜灯 30 分钟超时 */
} timer_id_t;

/* OTA 升级参数最大长度 */
#define OTA_URL_MAX_LEN         256     /* 固件下载地址最大长度 */
#define OTA_TASK_ID_MAX_LEN     16      /* 任务ID字符串最大长度 */
#define OTA_VERSION_MAX_LEN     16      /* 版本号最大长度 */

/* 统一消息结构 — 通过 FreeRTOS 队列在模块间传递 */
typedef struct {
    msg_type_t type;        /* 消息类型 */

    /* 按键事件字段 */
    button_id_t btn_id;
    button_event_t btn_evt;

    /* 命令字段 */
    command_id_t cmd_id;
    lamp_id_t lamp_id;
    int32_t value;          /* 亮度值 0~100 / 模式枚举 / 开关 0|1 */

    /* 定时器字段 */
    timer_id_t timer_id;

    /* OTA 字段 */
    char ota_url[OTA_URL_MAX_LEN];      /* 固件下载地址 */
    char ota_task_id[OTA_TASK_ID_MAX_LEN]; /* 任务ID */
    char ota_version[OTA_VERSION_MAX_LEN]; /* 目标版本号 */
} system_msg_t;

/* ═══════════════════════════════════════════════════
 * 灯状态结构
 * ═══════════════════════════════════════════════════ */

typedef struct {
    bool switch_upper;      /* 上灯开关 */
    bool switch_lower;      /* 下灯开关 */
    uint8_t bright_upper;   /* 上灯亮度 0~100 */
    uint8_t bright_lower;   /* 下灯亮度 0~100 */
} lamp_status_t;

/* ═══════════════════════════════════════════════════
 * 亮度与时序常量
 * ═══════════════════════════════════════════════════ */

#define MIN_BRIGHTNESS          5       /* 最暗亮度 (与夜灯模式一致) */
#define MAX_BRIGHTNESS          100     /* 最亮亮度 */
#define DEFAULT_BRIGHTNESS      100     /* 默认亮度 */
#define NIGHT_BRIGHTNESS        5       /* 夜灯模式上灯亮度 */

/* PWM 参数 */
#define PWM_FREQUENCY_HZ        1000    /* LED PWM 频率 1kHz (无可见闪烁) */
#define PWM_RESOLUTION_BIT      10      /* 10bit 分辨率, 占空比 0~1023 */

/* 缓起缓灭时间 */
#define FADE_IN_TIME_MS         1500    /* 开灯缓起 1.5s */
#define FADE_OUT_TIME_MS        1000    /* 关灯缓灭 1.0s */
#define MODE_FADE_TIME_MS       500     /* 模式切换过渡 0.5s */
#define DOUBLE_CLICK_FADE_MS    200     /* 双击极值切换 0.2s */

/* 长按调光参数 */
#define DIM_STEP_INTERVAL_MS    20      /* 调光步进间隔 */
#define DIM_STEP_PCT            1       /* 每步亮度变化 1% */

/* 按键时序参数 */
#define DEBOUNCE_MS             20      /* 消抖时间 */
#define LONG_PRESS_THRESHOLD   800     /* 长按判定阈值 */
#define DOUBLE_CLICK_WINDOW     300     /* 双击间隔窗口 */

/* 夜灯定时 */
#define NIGHT_LAMP_TIMEOUT_MS   (30 * 60 * 1000)  /* 30 分钟 */

/* 上电指示灯时间 */
#define POWER_ON_INDICATOR_MS   1000    /* 上电全亮 1s */

/* 按键轮询周期 */
#define BUTTON_POLL_MS          10      /* 按键扫描间隔 10ms */

/* ═══════════════════════════════════════════════════
 * Wi-Fi 配网参数
 *
 * WiFi 凭据通过 SmartConfig (ESP-TOUCH) 获取，存储在 NVS 中
 * 不再硬编码 SSID/密码
 * ═══════════════════════════════════════════════════ */

#define WIFI_MAX_RETRY          5       /* 直接连接最大重试次数 */

/* ═══════════════════════════════════════════════════
 * FastBee 物联网平台配置
 *   产品级参数 (编译时固定):
 *     - 在 FastBee 平台创建产品，选择: 直连设备 + MQTT + JSON + 简单认证
 *     - 将下方宏填入平台生成的产品级参数
 *   设备级参数 (配网时动态获取，存 NVS):
 *     - userId: App 下发，标识设备归属的用户
 *     - deviceNum: App 下发或由 MAC 地址自动生成
 *   配网流程: 设备 AP 模式 → App 下发 → 存 NVS → 连 WiFi → 连 MQTT
 * ═══════════════════════════════════════════════════ */

#define FB_MQTT_HOST            "81.71.99.53"      /* MQTT 服务器地址 */
#define FB_MQTT_PORT            1883               /* MQTT 端口 (TLS=8883) */
#define FB_PRODUCT_ID           "136"              /* 产品编号 */
#define FB_MQTT_USERNAME        "FastBee"          /* 认证账号 */
#define FB_MQTT_PASSWORD        "P55WMKIT5563GT7Z" /* 认证密码 */
#define FB_FIRMWARE_VERSION     "1.0"             /* 固件版本 */
#define FB_OTA_BASE_URL         "https://www.fleetbee.top/prod-api" /* OTA 固件下载基础地址 */
/* FB_DEVICE_NUM 和 FB_USER_ID 不再硬编码，由 AP 配网时 App 下发，存入 NVS */

/* 物模型标识 — 必须与 FastBee 平台物模型定义一致 */
#define FB_PROP_POWER           "power"            /* 总开关: "1"=开 "0"=关 */
#define FB_PROP_MODE            "mode"             /* 模式: off/normal/reading/night */
#define FB_PROP_SWITCH_UPPER    "switch_upper"     /* 上灯开关 */
#define FB_PROP_SWITCH_LOWER    "switch_lower"     /* 下灯开关 */
#define FB_PROP_BRIGHT_UPPER    "bright_upper"     /* 上灯亮度 0~100 */
#define FB_PROP_BRIGHT_LOWER    "bright_lower"     /* 下灯亮度 0~100 */

#endif /* LAMP_TYPES_H */
