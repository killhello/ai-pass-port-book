// main/captive_portal.h —— 热点配网：AP + DNS + HTTP 配网页面
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 配网完成回调
typedef void (*captive_portal_cb_t)(bool success, void *user);

// 启动热点配网（AP 模式 + DNS 劫持 + HTTP 配网页面）
// AP 名称: ESP-WiFi, 无密码
esp_err_t captive_portal_start(captive_portal_cb_t cb, void *user);

// 停止热点配网
esp_err_t captive_portal_stop(void);

// 是否正在运行
bool captive_portal_is_running(void);
