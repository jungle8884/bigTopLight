/**
 * indicator.h — 指示灯控制模块接口
 *
 * 5 个指示灯 GPIO 输出, 跟随系统状态变化
 */
#ifndef INDICATOR_H
#define INDICATOR_H

#include "lamp_types.h"

/** 初始化指示灯 GPIO (输出模式, 初始全灭) */
void indicator_init(void);

/** 上电序列: 全部指示灯亮 1 秒后熄灭 */
void indicator_power_on_sequence(void);

/** 根据系统状态更新所有指示灯 */
void indicator_update(system_state_t state, const lamp_status_t *status);

#endif /* INDICATOR_H */
