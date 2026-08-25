// main/wifi_sta.c —— WiFi STA 模块:扫描(所有网络) + 动态连接(支持密码)。
// NVS 保存上次连接的 SSID+密码,启动时自动重连。
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

// 同步扫描并返回结果。阻塞直到扫描完成或超时。
// 返回实际找到的网络数(0=失败/无网络)
int wifi_sta_scan_and_get(wifi_ap_info_t *out, int max_count, int timeout_ms) {
    if (wifi_init_once() != ESP_OK) return 0;

    // 等待 WiFi 完全就绪
    vTaskDelay(pdMS_TO_TICKS(500));

    // 最多尝试 2 次扫描，使用最简单的配置
    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "开始第 %d 次扫描...", attempt);

        // 使用最简单的配置：默认参数，被动扫描更稳
        wifi_scan_config_t scan_cfg = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_PASSIVE,
            .scan_time.passive = 200,  // 每信道 200ms
        };
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "第 %d 次扫描启动失败: %s", attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "第 %d 次扫描发现 %d 个 AP", attempt, ap_count);
        
        if (ap_count > 0) {
            uint16_t fetch = ap_count < WIFI_SCAN_MAX ? ap_count : WIFI_SCAN_MAX;
            wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
            if (!records) return 0;

            esp_wifi_scan_get_ap_records(&fetch, records);

            int count = 0;
            for (int i = 0; i < fetch && count < max_count; i++) {
                strlcpy(out[count].ssid, (const char *)records[i].ssid, sizeof(out[count].ssid));
                out[count].rssi = records[i].rssi;
                out[count].authmode = records[i].authmode;
                count++;
            }

            free(records);
            s_scan_count = count;
            memcpy(s_scan_results, out, count * sizeof(wifi_ap_info_t));
            ESP_LOGI(TAG, "扫描成功: %d 个网络", count);
            return count;
        }

        ESP_LOGW(TAG, "第 %d 次扫描无结果，重试...", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "扫描均无结果");
    return 0;
}

// 兼容旧 API:异步启动扫描(内部改为同步)
bool wifi_sta_start_scan(void) {
    // 这里不再真正启动异步扫描，保留接口兼容
    // 实际扫描在 wifi_sta_scan_and_get 里完成
    if (wifi_init_once() != ESP_OK) return false;
    ESP_LOGI(TAG, "WiFi 就绪，可扫描");
    return true;
}

// 兼容旧 API:获取结果(内部直接同步扫描)
int wifi_sta_get_scan_results(wifi_ap_info_t *out, int max_count, int timeout_ms) {
    (void)timeout_ms;
    return wifi_sta_scan_and_get(out, max_count, timeout_ms);
}

esp_err_t wifi_sta_connect_to(const char *ssid, const char *password) {
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
    if (password && password[0]) {
        strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "正在连接 %s ...", ssid);

    xSemaphoreTake(s_got_ip, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接启动失败: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) == pdTRUE) {
        strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", ssid);
            if (password && password[0]) {
                nvs_set_str(h, "pass", password);
            } else {
                nvs_erase_key(h, "pass");
            }
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "已连接 %s", ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "连接 %s 超时", ssid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_connect(void) {
    char saved_ssid[33] = { 0 };
    char saved_pass[65] = { 0 };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(saved_ssid);
        nvs_get_str(h, "ssid", saved_ssid, &len);
        len = sizeof(saved_pass);
        nvs_get_str(h, "pass", saved_pass, &len);
        nvs_close(h);
    }

    if (saved_ssid[0] != '\0') {
        return wifi_sta_connect_to(saved_ssid, saved_pass[0] ? saved_pass : NULL);
    }

    return wifi_sta_connect_to(WIFI_SSID, WIFI_PASS);
}

bool wifi_sta_is_connected(void) {
    return s_connected;
}

const char *wifi_sta_current_ssid(void) {
    return s_current_ssid;
}