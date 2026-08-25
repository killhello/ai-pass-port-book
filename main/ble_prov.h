// main/ble_prov.h —— 蓝牙(BLE/NimBLE)配网
// 手机用 BLE 调试工具(nRF Connect 等)连接本设备，向配网特征写入两行文本：
//   第1行: WiFi 名称(SSID)
//   第2行: 密码(开放网络留空)
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// 启动 BLE 配网广播（调用前建议先 wifi_sta_stop() 释放 WiFi 内存）
esp_err_t ble_prov_start(void);

// 停止并反初始化 BLE（收到凭证、连接 WiFi 前必须调用以释放内存）
esp_err_t ble_prov_stop(void);

// 是否正在运行
bool ble_prov_is_running(void);

// 取走收到的凭证（读取后内部标志清除，只返回一次 true；未收到返回 false）
bool ble_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
