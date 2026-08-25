// main/wifi_sta.h —— WiFi STA 模块：扫描 + 连接 + NVS 存储 + 事件回调
// 参考 esp32-wifi-manager 的稳健模式：事件驱动 + NVS 互斥 + AP 回退
#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"
#include <stdbool.h>

// WiFi 扫描结果
#define WIFI_SCAN_MAX  20

typedef struct {
    char             ssid[33];
    int8_t           rssi;
    wifi_auth_mode_t authmode;
    uint8_t          primary;   // 信道
} wifi_ap_info_t;

// 事件类型
typedef enum {
    WIFI_STA_EVT_CONNECTED,     // 已连接并获取 IP
    WIFI_STA_EVT_DISCONNECTED,  // 断开连接
    WIFI_STA_EVT_SCAN_DONE,     // 扫描完成
    WIFI_STA_EVT_FAIL,          // 连接失败
} wifi_sta_evt_t;

// 事件回调签名
typedef void (*wifi_sta_cb_t)(wifi_sta_evt_t evt, void *data, void *user);

// 启动扫描（同步阻塞，返回 AP 数量）
int wifi_sta_scan(wifi_ap_info_t *out, int max_count);

// 连接指定 SSID（支持密码，阻塞直到拿到 IP 或超时）
// password 为 NULL/空 = 开放网络
esp_err_t wifi_sta_connect(const char *ssid, const char *password);

// 断开当前连接
esp_err_t wifi_sta_do_disconnect(void);

// 是否已连接并有 IP
bool wifi_sta_is_connected(void);

// 获取当前连接的 SSID
const char *wifi_sta_current_ssid(void);

// 注册事件回调（可在任意时刻调用，线程安全）
void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user);

// 注销回调
void wifi_sta_unregister_cb(wifi_sta_cb_t cb);

// 初始化 WiFi 栈（内部幂等，首次调用时创建 netif/event/task）
esp_err_t wifi_sta_init(void);

// 启动 AP 回退模式（供配网用，可选）
esp_err_t wifi_sta_start_ap_fallback(const char *ap_ssid, const char *ap_pass);

// 使用 NVS 保存的凭证自动连接（兼容旧 API 无参调用）
esp_err_t wifi_sta_connect_default(void);

// ===== SmartConfig (ESP-Touch) =====
typedef enum {
    SC_STATUS_IDLE,
    SC_STATUS_FIND_CHANNEL,
    SC_STATUS_GETTING_SSID_PSWD,
    SC_STATUS_LINK,
    SC_STATUS_LINK_OVER,
    SC_STATUS_FAIL,
} smartconfig_status_t;

// SmartConfig 回调签名
typedef void (*smartconfig_cb_t)(smartconfig_status_t status, void *user);

// 启动 SmartConfig（ESP-Touch 模式，阻塞直到成功/失败/超时）
// 成功时自动保存凭证到 NVS 并连接
esp_err_t wifi_sta_smartconfig_start(smartconfig_cb_t cb, void *user, int timeout_ms);

// 停止 SmartConfig
void wifi_sta_smartconfig_stop(void);

// ===== SmartConfig (ESP-Touch) =====
typedef enum {
    SC_STATUS_IDLE,
    SC_STATUS_FIND_CHANNEL,
    SC_STATUS_GETTING_SSID_PSWD,
    SC_STATUS_LINK,
    SC_STATUS_LINK_OVER,
    SC_STATUS_FAIL,
} smartconfig_status_t;

// SmartConfig 回调签名
typedef void (*smartconfig_cb_t)(smartconfig_status_t status, void *user);

// 启动 SmartConfig（ESP-Touch 模式，阻塞直到成功/失败/超时）
// 成功时自动保存凭证到 NVS 并连接
esp_err_t wifi_sta_smartconfig_start(smartconfig_cb_t cb, void *user, int timeout_ms);

// 停止 SmartConfig
void wifi_sta_smartconfig_stop(void);

// 是否正在进行 SmartConfig
bool wifi_sta_smartconfig_is_running(void);

// ===== BLE Provisioning =====
typedef enum {
    BLE_PROV_STATUS_IDLE,
    BLE_PROV_STATUS_STARTING,
    BLE_PROV_STATUS_WAITING_CLIENT,
    BLE_PROV_STATUS_RECEIVING_CREDS,
    BLE_PROV_STATUS_CONNECTING_WIFI,
    BLE_PROV_STATUS_SUCCESS,
    BLE_PROV_STATUS_FAIL,
} ble_prov_status_t;

// BLE Provisioning 回调签名
typedef void (*ble_prov_cb_t)(ble_prov_status_t status, void *user);

// 启动 BLE Provisioning（阻塞直到成功/失败/超时）
// 手机端使用 "ESP Provision" App 或微信小程序 "ESP BLE Provision"
// 成功时自动保存凭证到 NVS 并连接
esp_err_t wifi_sta_ble_prov_start(ble_prov_cb_t cb, void *user, int timeout_ms);

// 停止 BLE Provisioning
void wifi_sta_ble_prov_stop(void);

// 是否正在进行 BLE Provisioning
bool wifi_sta_ble_prov_is_running(void);