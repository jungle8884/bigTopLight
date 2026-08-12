/**
 * state_machine.h — 状态机 + 指示灯控制模块接口
 *
 * 职责:
 *   1. 拥有系统事件队列 (button/mqtt/timer 统一投递至此)
 *   2. 运行 lamp_task: 消费事件 → 状态转移 → 驱动 PWM/指示灯/存储/MQTT
 *   3. 管理指示灯 GPIO (5 个 LED 跟随系统状态)
 */
#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "lamp_types.h"

/**
 * 初始化状态机:
 *   - 创建事件队列
 *   - 从 NVS 加载保存的亮度/开关
 *   - 初始化 PWM 驱动
 *   - 初始化指示灯 GPIO
 *   - 执行上电指示灯全亮 1s 序列
 *   - 创建内部定时器 (调光步进 + 夜灯 30min)
 */
void state_machine_init(void);

/**
 * 启动状态机任务 (lamp_task, 优先级 7)
 */
void state_machine_start_task(void);

/**
 * 获取事件队列句柄
 * button / mqtt / timer 模块通过此队列投递事件
 */
QueueHandle_t state_machine_get_event_queue(void);

/**
 * 向队列发送事件 (便捷封装)
 */
void state_machine_send_event(system_msg_t *msg);

/**
 * 获取当前系统状态
 */
system_state_t state_machine_get_state(void);

/**
 * 获取当前灯状态 (开关 + 亮度)
 */
void state_machine_get_status(lamp_status_t *status);

/**
 * 调光步进 (由 timer.c 的 dim 定时器回调直接调用)
 * 内部根据当前调光方向递增/递减亮度并更新 PWM
 */
void state_machine_dim_step(void);

#endif /* STATE_MACHINE_H */
