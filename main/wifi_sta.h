// main/wifi_sta.h —— WiFi STA 阻塞式连接(仅供 worker 任务调用,勿在 LVGL/按键上下文用)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 初始化 esp_netif/事件循环/WIFI 并连接到 ai_config.h 配置的 AP。
// 阻塞直至拿到 IP 或超时。重复调用:已连接直接返回 OK;未连接则重试。
esp_err_t wifi_sta_connect(void);

// 当前是否已拿到 IP。
bool wifi_sta_is_connected(void);
