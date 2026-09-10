/**
 * mqtt_app.h — FastBee 平台 MQTT 客户端模块接口
 *
 * 对接 FastBee 物联网平台，使用简单认证 (Simple Auth) 模式
 *
 * ═══ FastBee 主题结构 ═══
 *   前缀: /{productId}/{deviceNum}
 *
 *   订阅 (平台下发):
 *     /{productId}/{deviceNum}/function/get    — 功能指令 (物模型命令)
 *     /{productId}/{deviceNum}/property/get   — 属性查询
 *     /{productId}/{deviceNum}/info/get       — 设备信息查询
 *     /{productId}/{deviceNum}/monitor/get    — 监控控制
 *     /{productId}/{deviceNum}/ntp/get         — NTP 时间同步
 *     /{deviceNum}/http/upgrade/set           — OTA 升级指令 (注意: 主题格式不同)
 *
 *   发布 (设备上报):
 *     /{productId}/{deviceNum}/property/post  — 属性数据 (bright_upper, bright_lower)
 *     /{productId}/{deviceNum}/function/post  — 功能状态 (power, mode, switch_upper, switch_lower)
 *     /{productId}/{deviceNum}/info/post      — 设备信息 (含在线/离线状态)
 *     /{deviceNum}/http/upgrade/reply          — OTA 升级回复 (注意: 主题格式不同)
 *
 * ═══ MQTT 认证 (简单模式) ═══
 *   ClientId:  S&{deviceNum}&{productId}&{userId}
 *   Username:  平台认证账号
 *   Password:  平台认证密码 (明文)
 *
 * ═══ 消息格式 (物模型 JSON 数组) ═══
 *   属性上报:
 *     [{"id":"power","value":"1","remark":""},
 *      {"id":"mode","value":"reading","remark":""},
 *      {"id":"bright_upper","value":"80","remark":""}]
 *
 *   功能指令 (平台下发):
 *     [{"id":"power","value":"1"},
 *      {"id":"brightness_upper","value":"80"}]
 *
 *   设备信息:
 *     {"rssi":-55,"firmwareVersion":"1.0","status":3,"userId":"1",
 *      "summary":{"name":"XS-XLD5","chip":"ESP32-C3","version":"1.0"}}
 *
 *   LWT 遗嘱 (info/post 主题):
 *     {"status":4}   (status: 3=在线, 4=离线)
 */
#ifndef MQTT_APP_H
#define MQTT_APP_H

#include "lamp_types.h"

/**
 * 初始化并启动 FastBee MQTT 客户端
 *   - 连接 FastBee MQTT broker
 *   - 设置 ClientId / 用户名 / 密码 (简单认证)
 *   - 订阅 function/get 主题 (接收平台命令)
 *   - 设置 LWT 遗嘱 (info/post, {"status":4})
 *   - 连接成功后自动发布设备信息 (info/post)
 *   - 收到命令时解析 JSON 数组并投递到状态机队列
 */
void mqtt_app_start(void);

/**
 * 发布设备属性到 FastBee 平台
 *   - 被 state_machine 在状态变更时调用
 *   - 构建物模型 JSON 数组, 发布到 property/post 主题
 *   - 包含: power, mode, switch_upper, switch_lower, bright_upper, bright_lower
 */
void mqtt_publish_state(const lamp_status_t *status, system_state_t state);

void mqtt_publish_brightness(lamp_id_t lamp, uint8_t brightness);

/**
 * 发布 OTA 升级回复到 FastBee 平台
 *   - 发布到 upgrade/reply 主题
 *   - 由 ota_app.c 调用, 上报升级进度和结果
 *
 * @param payload JSON 消息体字符串
 */
void mqtt_publish_ota_reply(const char *payload);

#endif /* MQTT_APP_H */
