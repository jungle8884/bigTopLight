/**
 * storage.h — NVS 非易失存储模块接口
 *
 * 功能: 保存/恢复灯亮度与开关状态, 掉电不丢失
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "lamp_types.h"

/**
 * 初始化 NVS Flash (在所有 NVS 操作前必须调用)
 */
void storage_init(void);

/**
 * 从 NVS 加载保存的参数
 *   status — 输出: 加载的灯状态 (亮度+开关)
 *   首次使用 (key 不存在) 时设默认值: 亮度 100%, 开关关
 */
void storage_load(lamp_status_t *status);

/**
 * 保存单灯亮度到 NVS
 *   lamp       — 上灯/下灯
 *   brightness — 亮度 0~100
 */
void storage_save_brightness(lamp_id_t lamp, uint8_t brightness);

/**
 * 保存开关状态到 NVS
 *   sw_upper — 上灯开关
 *   sw_lower — 下灯开关
 */
void storage_save_switch_state(bool sw_upper, bool sw_lower);

#endif /* STORAGE_H */
