/**
 * ota_app.h — OTA 固件升级模块
 *
 * FastBee OTA 协议 (HTTPS 方式):
 *   下发主题: /{serialNumber}/http/upgrade/set    (云端 -> 设备)
 *   回复主题: /{serialNumber}/http/upgrade/reply   (设备 -> 云端)
 *
 * 下发消息: {"taskId":26, "url":"/profile/iot/1/xxx.bin", "version":1.2, "status":1}
 * 进度回复: {"taskId":26, "progress":50, "version":"1.2", "status":2}
 * 完成回复: {"taskId":26, "progress":100, "version":"1.2", "status":3}
 *
 * url 为相对路径, 设备拼接 FB_OTA_BASE_URL 后下载
 * status: 0=等待 1=已发送 2=升级中 3=成功 4=失败
 */
#ifndef OTA_APP_H
#define OTA_APP_H

#include "esp_err.h"
#include "lamp_types.h"

/**
 * 启动 OTA 升级 (阻塞函数, 在独立任务中调用)
 *
 * @param url        固件下载地址 (完整 HTTP/HTTPS URL)
 * @param task_id    平台任务 ID (用于回复)
 * @param version    目标固件版本号
 * @return ESP_OK 成功 (成功后设备会重启, 不返回)
 *         其他 失败
 */
esp_err_t ota_app_start(const char *url, const char *task_id, const char *version);

/**
 * 获取当前固件版本号
 */
const char *ota_app_get_version(void);

/**
 * 判断是否需要升级
 * @param target_version 目标版本号
 * @return true 需要升级, false 已是最新
 */
bool ota_app_need_upgrade(const char *target_version);

#endif /* OTA_APP_H */
