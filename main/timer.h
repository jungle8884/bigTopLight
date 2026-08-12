/**
 * timer.h — 定时器管理模块接口
 *
 * 管理两个 esp_timer:
 *   1. 调光步进定时器 (周期 20ms, 触发 state_machine_dim_step 回调)
 *   2. 夜灯超时定时器 (单次 30min, 触发状态机 TIMER 事件)
 */
#ifndef TIMER_H
#define TIMER_H

#include "lamp_types.h"

/** 创建定时器 (在 state_machine_init 中调用) */
void timer_init(void);

/** 启动调光定时器 (周期 20ms) */
void timer_start_dim(void);

/** 停止调光定时器 */
void timer_stop_dim(void);

/** 启动夜灯 30min 倒计时 (单次) */
void timer_start_night(void);

/** 停止夜灯倒计时 */
void timer_stop_night(void);

#endif /* TIMER_H */
