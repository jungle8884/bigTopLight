/**
 * button.h — 按键检测模块接口
 *
 * 功能: 消抖 + 短按/长按/双击识别, 通过 FreeRTOS 队列发送事件
 */
#ifndef BUTTON_H
#define BUTTON_H

#include "lamp_types.h"

/**
 * 初始化按键 GPIO (5 个按键引脚配置为输入 + 上拉)
 */
void button_init(void);

/**
 * 创建按键检测任务 (button_task)
 * 任务: 每 10ms 轮询 5 个按键 GPIO, 识别短按/长按/双击, 发送到事件队列
 */
void button_start_task(void);

#endif /* BUTTON_H */
