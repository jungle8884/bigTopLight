/**
 * wifi_app.h — Wi-Fi 连接管理模块接口 (AP 模式配网)
 *
 * 兼容 FastBee App 配网流程:
 *   1. 设备进入 AP 模式，开启热点 FastBee-XXXX，IP 192.168.4.1
 *   2. HTTP Server 提供 /status (GET) 和 /config (POST) 接口
 *   3. App 检测设备后，下发 SSID + password + userId + deviceNum 等参数
 *   4. 设备保存参数到 NVS，切换 STA 模式连接 WiFi
 *   5. 连接成功后回调启动 MQTT
 */
#ifndef WIFI_APP_H
#define WIFI_APP_H

/**
 * 启动 Wi-Fi 连接管理
 *   - 检查 NVS 是否有已保存的 WiFi 凭据
 *   - 有 → STA 模式直接连接
 *   - 无 → AP 模式开启热点 + HTTP Server 等待配网
 *   - 连接失败超限 → 回退 AP 模式重新配网
 *   - 连接成功后回调启动 MQTT (由 wifi_app_set_connected_callback 注册)
 */
void wifi_app_start(void);

/**
 * 手动触发 AP 配网 (如按键长按)
 *   - 断开当前连接，清除旧凭据，进入 AP 模式
 */
void wifi_app_start_apconfig(void);

/**
 * 注册 WiFi 连接成功回调
 *   cb — 获取 IP 后调用的回调函数 (通常用于启动 MQTT)
 */
void wifi_app_set_connected_callback(void (*cb)(void));

#endif /* WIFI_APP_H */
