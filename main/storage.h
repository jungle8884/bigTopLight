/**
 * storage.h — NVS 非易失存储模块接口
 *
 * 功能: 保存/恢复灯亮度与开关状态, 掉电不丢失
 *       保存/恢复 WiFi 凭据 + 设备信息 (userId, deviceNum)
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "lamp_types.h"
#include <string.h>

/* ═══════════════════════════════════════════════════
 * 灯状态存取
 * ═══════════════════════════════════════════════════ */

void storage_init(void);
void storage_load(lamp_status_t *status);
void storage_save_brightness(lamp_id_t lamp, uint8_t brightness);
void storage_save_switch_state(bool sw_upper, bool sw_lower);

/* ═══════════════════════════════════════════════════
 * WiFi 凭据存取 (AP 配网)
 * ═══════════════════════════════════════════════════ */

/**
 * 从 NVS 加载 WiFi 凭据
 *   ssid     — 输出: WiFi SSID (需 >= 33 字节)
 *   password — 输出: WiFi 密码 (需 >= 65 字节)
 *   ssid_len / pwd_len — 缓冲区大小
 *   返回: true=找到凭据, false=无凭据 (首次使用)
 */
bool storage_load_wifi_creds(char *ssid, char *password,
                             size_t ssid_len, size_t pwd_len);

/**
 * 保存 WiFi 凭据到 NVS
 */
void storage_save_wifi_creds(const char *ssid, const char *password);

/**
 * 清除 NVS 中的 WiFi 凭据
 */
void storage_clear_wifi(void);

/* ═══════════════════════════════════════════════════
 * 设备信息存取 (AP 配网下发)
 * ═══════════════════════════════════════════════════ */

/**
 * 从 NVS 加载设备信息 (配网时 App 下发)
 *   user_id    — 输出: 用户 ID (需 >= 16 字节)
 *   device_num — 输出: 设备编号 (需 >= 33 字节)
 *   auth_code  — 输出: 授权码 (需 >= 33 字节, 可为空)
 *   返回: true=找到设备信息, false=无 (未配网)
 */
bool storage_load_device_info(char *user_id, char *device_num,
                              char *auth_code);

/**
 * 保存设备信息到 NVS (配网成功后调用)
 */
void storage_save_device_info(const char *user_id,
                              const char *device_num,
                              const char *auth_code);

#endif /* STORAGE_H */
