/**
 * button.c — 按键检测模块实现
 *
 * 检测逻辑: 10ms 轮询 → 消抖(20ms) → 时序判定
 *   - 短按: 按下后 800ms 内释放
 *   - 长按: 按下超过 800ms → 发 LONG_PRESS_START, 释放时发 LONG_PRESS_RELEASE
 *   - 双击: 第一次短按释放后 300ms 内再次按下 → 发 DOUBLE_CLICK
 *
 * 数据流: GPIO 电平 → 消抖状态机 → 事件 → xQueueSend → 状态机队列
 */
#include "button.h"
#include "state_machine.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "BUTTON";

/* 按键 GPIO 引脚表 (索引对应 button_id_t 枚举) */
static const int btn_gpios[BTN_COUNT] = {
    GPIO_BTN_POWER,
    GPIO_BTN_UPPER,
    GPIO_BTN_LOWER,
    GPIO_BTN_READING,
    GPIO_BTN_NIGHT,
};

/* ── 单个按键的检测状态机 ── */
typedef enum {
    BTN_ST_IDLE = 0,       /* 空闲: 等待按下 */
    BTN_ST_DEBOUNCE,       /* 消抖中: 按下后等待消抖确认 */
    BTN_ST_PRESSED,        /* 已按下: 计时判断长按/短按 */
    BTN_ST_WAIT_DOUBLE,    /* 等待双击: 短按释放后等 300ms */
    BTN_ST_DEBOUNCE2,      /* 双击第二次消抖 */
    BTN_ST_PRESSED2,       /* 双击第二次按下 */
    BTN_ST_LONG_PRESS,     /* 长按持续中: 等待释放 */
} btn_state_t;

/* 每个按键的运行时状态 */
typedef struct {
    btn_state_t state;       /* 当前状态 */
    uint32_t    press_time;  /* 按下时刻 (系统 tick) */
    uint32_t    release_time;/* 释放时刻 */
} btn_runtime_t;

static btn_runtime_t btn_rt[BTN_COUNT];

/* ── 发送按键事件到状态机统一队列 ── */
static void send_event(button_id_t id, button_event_t evt)
{
    system_msg_t msg = {
        .type    = MSG_BUTTON,
        .btn_id  = id,
        .btn_evt = evt,
    };
    state_machine_send_event(&msg);
}

/* ── 读取按键 GPIO 电平 (active low: 0=按下, 1=松开) ── */
static inline bool btn_is_pressed(button_id_t id)
{
    return gpio_get_level(btn_gpios[id]) == 0;
}

/* ── 获取当前系统 tick (ms) ── */
static inline uint32_t get_tick_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* ═══════════════════════════════════════════════════
 * 单按键状态机处理 (每 10ms 调用一次)
 * ═══════════════════════════════════════════════════ */
static void button_process(button_id_t id)
{
    btn_runtime_t *rt = &btn_rt[id];
    bool pressed = btn_is_pressed(id);
    uint32_t now = get_tick_ms();

    switch (rt->state) {

    /* ── 空闲: 检测按下 ── */
    case BTN_ST_IDLE:
        if (pressed) {
            rt->state = BTN_ST_DEBOUNCE;
            rt->press_time = now;
        }
        break;

    /* ── 消抖: 等 20ms 确认 ── */
    case BTN_ST_DEBOUNCE:
        if (now - rt->press_time >= DEBOUNCE_MS) {
            if (pressed) {
                /* 确认按下, 进入计时阶段 */
                rt->state = BTN_ST_PRESSED;
                rt->press_time = now;
            } else {
                /* 消抖期内松开, 判定为抖动 */
                rt->state = BTN_ST_IDLE;
            }
        }
        break;

    /* ── 已按下: 判断长按还是短按 ── */
    case BTN_ST_PRESSED:
        if (!pressed) {
            /* 800ms 内释放 → 进入双击等待窗口 */
            rt->state = BTN_ST_WAIT_DOUBLE;
            rt->release_time = now;
        } else if (now - rt->press_time >= LONG_PRESS_THRESHOLD) {
            /* 超过 800ms 仍按下 → 长按开始 */
            rt->state = BTN_ST_LONG_PRESS;
            send_event(id, BTN_EVT_LONG_PRESS_START);
        }
        break;

    /* ── 双击等待: 300ms 内是否有第二次按下 ── */
    case BTN_ST_WAIT_DOUBLE:
        if (pressed) {
            /* 窗口内再次按下 → 双击第二次消抖 */
            rt->state = BTN_ST_DEBOUNCE2;
            rt->press_time = now;
        } else if (now - rt->release_time >= DOUBLE_CLICK_WINDOW) {
            /* 窗口超时, 判定为短按 */
            send_event(id, BTN_EVT_SHORT_PRESS);
            rt->state = BTN_ST_IDLE;
        }
        break;

    /* ── 双击第二次消抖 ── */
    case BTN_ST_DEBOUNCE2:
        if (now - rt->press_time >= DEBOUNCE_MS) {
            if (pressed) {
                rt->state = BTN_ST_PRESSED2;
                rt->press_time = now;
            } else {
                /* 抖动, 回退到双击等待 */
                rt->state = BTN_ST_WAIT_DOUBLE;
                rt->release_time = now;
            }
        }
        break;

    /* ── 双击第二次按下: 等待释放 ── */
    case BTN_ST_PRESSED2:
        if (!pressed) {
            /* 双击确认 */
            send_event(id, BTN_EVT_DOUBLE_CLICK);
            rt->state = BTN_ST_IDLE;
        }
        break;

    /* ── 长按持续中: 等待释放 ── */
    case BTN_ST_LONG_PRESS:
        if (!pressed) {
            send_event(id, BTN_EVT_LONG_PRESS_RELEASE);
            rt->state = BTN_ST_IDLE;
        }
        break;

    default:
        rt->state = BTN_ST_IDLE;
        break;
    }
}

/* ═══════════════════════════════════════════════════
 * 按键检测任务 (button_task)
 * ═══════════════════════════════════════════════════ */
static void button_task(void *arg)
{
    ESP_LOGI(TAG, "button task started, poll=%dms", BUTTON_POLL_MS);

    /* 上一次处理时刻 */
    uint32_t last_tick = get_tick_ms();

    while (1) {
        uint32_t now = get_tick_ms();

        /* 每 10ms 处理一次所有按键 */
        if (now - last_tick >= BUTTON_POLL_MS) {
            last_tick = now;
            for (int i = 0; i < BTN_COUNT; i++) {
                button_process((button_id_t)i);
            }
        }

        /* 短延时, 让出 CPU */
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS) / 2);
    }
}

/* ═══════════════════════════════════════════════════
 * 公共接口
 * ═══════════════════════════════════════════════════ */

void button_init(void)
{
    /* 配置 5 个按键 GPIO 为输入 + 内部上拉 (active low) */
    gpio_config_t io_conf = {
        .pin_bit_mask = 0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,  /* 轮询模式, 不用中断 */  
    };

    for (int i = 0; i < BTN_COUNT; i++) { 
        io_conf.pin_bit_mask |= (1ULL << btn_gpios[i]);
    }

    gpio_config(&io_conf);

    /* 初始化运行时状态 */
    memset(btn_rt, 0, sizeof(btn_rt));

    ESP_LOGI(TAG, "button module initialized (%d buttons)", BTN_COUNT);
}

void button_start_task(void)
{
    /* 创建按键检测任务, 优先级 5 (中等) */
    xTaskCreate(button_task, "button_task", 3072, NULL, 5, NULL);
}
