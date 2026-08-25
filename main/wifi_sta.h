// main/wifi_sta.h —— WiFi STA 模块:扫描(所有网络) + 动态连接(支持密码)。
#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

// WiFi 扫描结果(所有网络)
#define WIFI_SCAN_MAX  20

typedef struct {
    char             ssid[33];
    int8_t           rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_info_t;

// 启动异步扫描,完成后回调。
// 返回: true=扫描已启动, false=启动失败
bool wifi_sta_start_scan(void);

// 等待扫描完成(阻塞,最长 timeout_ms)。调用前需先 start_scan。
// 结果写入 out 数组,返回实际数量。0=无结果。
int wifi_sta_get_scan_results(wifi_ap_info_t *out, int max_count, int timeout_ms);

// 连接到指定 SSID(支持密码)。阻塞直至拿到 IP 或超时。
// password 为 NULL 或空串 = 开放网络。
// 重复调用:已连接同一 SSID 直接返回 OK。
esp_err_t wifi_sta_connect_to(const char *ssid, const char *password);

// 连接到 ai_config 中配置的 SSN(兼容旧逻辑,从 NVS 或默认读取)。
esp_err_t wifi_sta_connect(void);

// 当前是否已拿到 IP。
bool wifi_sta_is_connected(void);

// 获取当前连接的 SSID(空串=未连接)。
const char *wifi_sta_current_ssid(void);