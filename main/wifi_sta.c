// main/wifi_sta.c —— WiFi STA 模块:扫描 + 动态连接(仅开放网络)。
// NVS 保存上次连接的 SSID,启动时自动重连。
#include "wifi_sta.h"
#include "ai_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// strlcpy 兼容层(ESP-IDF 无 strlcpy)
static inline size_t strlcpy_local(char *dst, const char *src, size_t dstsize) {
    if (dstsize == 0) return strlen(src);
    size_t n = strlen(src);
    if (n >= dstsize) n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return strlen(src);
}

#define strlcpy(dst, src, dstsize) strlcpy_local(dst, src, dstsize)

static const char *TAG = "wifi_sta";
static const char *NVS_NAMESPACE = "wifi";

static SemaphoreHandle_t s_got_ip;
static SemaphoreHandle_t s_scan_done;
static volatile bool s_connected;
static bool s_inited;
static char s_current_ssid[33];
static wifi_ap_info_t s_scan_results[WIFI_SCAN_MAX];
static int s_scan_count;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "WiFi 断开");
        // 不自动重连——由用户决定
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xSemaphoreGive(s_scan_done);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xSemaphoreGive(s_got_ip);
        ESP_LOGI(TAG, "已获取 IP");
    }
}

static esp_err_t wifi_init_once(void) {
    if (s_inited) return ESP_OK;

    s_got_ip = xSemaphoreCreateBinary();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_got_ip || !s_scan_done) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    ESP_LOGI(TAG, "WiFi 初始化完成");
    return ESP_OK;
}

bool wifi_sta_start_scan(void) {
    if (wifi_init_once() != ESP_OK) return false;

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "WiFi 扫描已启动");
    return true;
}

int wifi_sta_get_scan_results(wifi_ap_info_t *out, int max_count, int timeout_ms) {
    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "扫描超时");
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGI(TAG, "未发现 WiFi 网络");
        return 0;
    }

    uint16_t fetch = ap_count < WIFI_SCAN_MAX ? ap_count : WIFI_SCAN_MAX;
    wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!records) return 0;

    esp_wifi_scan_get_ap_records(&fetch, records);

    int count = 0;
    for (int i = 0; i < fetch && count < max_count; i++) {
        // 只保留开放网络(无密码)
        if (records[i].authmode == WIFI_AUTH_OPEN) {
            strlcpy(out[count].ssid, (const char *)records[i].ssid, sizeof(out[count].ssid));
            out[count].rssi = records[i].rssi;
            out[count].authmode = records[i].authmode;
            count++;
        }
    }

    free(records);
    s_scan_count = count;
    memcpy(s_scan_results, out, count * sizeof(wifi_ap_info_t));
    ESP_LOGI(TAG, "扫描完成: %d 个开放网络(共 %d 个 AP)", count, ap_count);
    return count;
}

esp_err_t wifi_sta_connect_to(const char *ssid) {
    if (wifi_init_once() != ESP_OK) return ESP_ERR_NO_MEM;

    // 已连接同一 SSID
    if (s_connected && strcmp(s_current_ssid, ssid) == 0) return ESP_OK;

    // 断开当前连接
    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "正在连接 %s ...", ssid);

    // 重置信号量
    xSemaphoreTake(s_got_ip, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 等待 IP
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) == pdTRUE) {
        strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
        // 保存到 NVS
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "已连接 %s", ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "连接 %s 超时", ssid);
    return ESP_ERR_TIMEOUT;
}

// 从 NVS 读取保存的 SSID 并连接(兼容旧逻辑)
esp_err_t wifi_sta_connect(void) {
    char saved_ssid[33] = { 0 };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(saved_ssid);
        nvs_get_str(h, "ssid", saved_ssid, &len);
        nvs_close(h);
    }

    if (saved_ssid[0] != '\0') {
        return wifi_sta_connect_to(saved_ssid);
    }

    // 无保存的 SSID,尝试默认
    return wifi_sta_connect_to(WIFI_SSID);
}

bool wifi_sta_is_connected(void) {
    return s_connected;
}

const char *wifi_sta_current_ssid(void) {
    return s_current_ssid;
}
