// main/wifi_sta.h —— WiFi STA 模块：扫描 + 连接 + NVS 存储
#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

#define WIFI_SCAN_MAX  20

typedef struct {
    char             ssid[33];
    int8_t           rssi;
    wifi_auth_mode_t authmode;
    uint8_t          primary;
} wifi_ap_info_t;

typedef enum {
    WIFI_STA_EVT_CONNECTED,
    WIFI_STA_EVT_DISCONNECTED,
    WIFI_STA_EVT_SCAN_DONE,
    WIFI_STA_EVT_FAIL,
} wifi_sta_evt_t;

typedef void (*wifi_sta_cb_t)(wifi_sta_evt_t evt, void *data, void *user);

int wifi_sta_scan(wifi_ap_info_t *out, int max_count);
esp_err_t wifi_sta_connect(const char *ssid, const char *password);
esp_err_t wifi_sta_do_disconnect(void);
bool wifi_sta_is_connected(void);
const char *wifi_sta_current_ssid(void);
void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user);
void wifi_sta_unregister_cb(wifi_sta_cb_t cb);
esp_err_t wifi_sta_init(void);
esp_err_t wifi_sta_connect_default(void);
