/**
 * timer.c — 定时器管理模块实现
 *
 * 调光定时器回调直接调用 state_machine_dim_step() (函数调用, 无队列延迟)
 * 夜灯定时器回调发送 MSG_TIMER 事件到状态机队列
 */
#include "timer.h"
#include "state_machine.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "TIMER";

/* 定时器句柄 */
static esp_timer_handle_t s_dim_timer;    /* 调光步进 (周期) */
static esp_timer_handle_t s_night_timer;  /* 夜灯超时 (单次) */

/* ── 调光步进回调: 直接调用状态机处理函数 ── */
static void dim_timer_callback(void *arg)
{
    /* state_machine 内部维护调光方向和目标灯, 此处仅触发步进 */
    state_machine_dim_step();
}

/* ── 夜灯超时回调: 发送定时器事件到队列 ── */
static void night_timer_callback(void *arg)
{
    system_msg_t msg = {
        .type      = MSG_TIMER,
        .timer_id  = TIMER_NIGHT_LAMP,
    };
    state_machine_send_event(&msg);
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void timer_init(void)
{
    /* 创建调光步进定时器 (周期性) */
    esp_timer_create_args_t dim_args = {
        .callback = dim_timer_callback,
        .name     = "dim_timer",
    };
    esp_timer_create(&dim_args, &s_dim_timer); 

    /* 创建夜灯超时定时器 (单次) */
    esp_timer_create_args_t night_args = {
        .callback = night_timer_callback,
        .name     = "night_timer",
    };
    esp_timer_create(&night_args, &s_night_timer);

    ESP_LOGI(TAG, "timers created (dim + night)");
}

void timer_start_dim(void)
{
    /* 启动周期性调光定时器, 每 20ms 触发一次 */
    esp_timer_start_periodic(s_dim_timer, DIM_STEP_INTERVAL_MS * 1000);
}

void timer_stop_dim(void)
{
    esp_timer_stop(s_dim_timer);
}

void timer_start_night(void)
{
    /* 启动单次夜灯定时器, 30 分钟后触发 */
    esp_timer_start_once(s_night_timer, NIGHT_LAMP_TIMEOUT_MS * 1000);
}

void timer_stop_night(void)
{
    esp_timer_stop(s_night_timer);
}
