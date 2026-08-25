// main/ble_gatt_server.h —— 自定义 BLE GATT 服务，用于 Web Bluetooth 配网
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BLE_GATT_EVT_CREDENTIALS_READY,  // SSID + 密码已收到，可以连接
} ble_gatt_server_evt_t;

typedef void (*ble_gatt_server_cb_t)(ble_gatt_server_evt_t evt, void *user);

// 初始化 BLE GATT 服务器（启动广播）
esp_err_t ble_gatt_server_init(ble_gatt_server_cb_t cb, void *user);

// 获取收到的凭证
void ble_gatt_server_get_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

// 重置凭证缓冲区
void ble_gatt_server_reset(void);

// 停止 BLE GATT 服务器
void ble_gatt_server_deinit(void);
