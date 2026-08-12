/**
 * pwm_drv.h — LEDC PWM 驱动模块接口
 *
 * 功能: 2 通道 LEDC PWM 输出, 支持硬件渐变 (缓起缓灭) 和直接设置
 */
#ifndef PWM_DRV_H
#define PWM_DRV_H

#include "lamp_types.h"
#include <inttypes.h>

/** 初始化 LEDC 定时器和 2 个 PWM 通道 (1kHz, 10bit) */
void pwm_init(void);

/** 设置灯亮度 (带渐变) */
void pwm_set_brightness(lamp_id_t lamp, uint8_t brightness, uint32_t fade_ms);

/** 关灯 (渐变到 0) */
void pwm_turn_off(lamp_id_t lamp, uint32_t fade_ms);

/** 立即设置占空比 (无渐变, 用于长按调光) */
void pwm_set_duty_immediate(lamp_id_t lamp, uint8_t brightness);

/** 获取当前亮度值 (逻辑值 0~100) */
uint8_t pwm_get_brightness(lamp_id_t lamp);

#endif /* PWM_DRV_H */
