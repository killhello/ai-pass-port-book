// main/wifi_sta.c —— WiFi STA 阻塞式连接。
// 事件回调和状态查询线程安全:信号量 + 原子布尔。
#include "wifi_sta.h"
#include "ai_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "wifi_sta";

static SemaphoreHandle_t s_got_ip;   // 拿到 IP 时 give
static volatile bool s_connected;
static bool s_inited;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "WiFi 断开,重试...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xSemaphoreGive(s_got_ip);
        ESP_LOGI(TAG, "已获取 IP");
    }
}

static esp_err_t wifi_init_once(void) {
    if (s_inited) return ESP_OK;

    s_got_ip = xSemaphoreCreateBinary();
    if (!s_got_ip) return ESP_ERR_NO_MEM;

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

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;   // 兼容 WPA/WPA2

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());               // START 事件里自动 connect

    s_inited = true;
    ESP_LOGI(TAG, "WiFi 初始化完成, 连接 %s ...", WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_sta_connect(void) {
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    if (s_connected) return ESP_OK;

    // 断线事件里会自动重连,这里只等 IP
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) == pdTRUE) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连接超时(%d ms)", WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void) {
    return s_connected;
}
