// main/wifi_sta.c —— WiFi STA 模块：扫描 + 连接 + NVS 存储
#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>

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
static const char *NVS_NS = "wifi_sta";

static SemaphoreHandle_t s_nvs_mutex;
static SemaphoreHandle_t s_got_ip;
static volatile bool s_connected = false;
static bool s_inited = false;
static bool s_once = false;   // netif/事件处理器只需创建一次, 反复 init/deinit 会泄漏
static char s_cur_ssid[33] = {0};

static wifi_sta_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

static void fire_evt(wifi_sta_evt_t evt, void *data) {
    if (s_user_cb) s_user_cb(evt, data, s_user_data);
}

static esp_err_t nvs_lock(TickType_t wait) {
    if (!s_nvs_mutex) s_nvs_mutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_nvs_mutex, wait) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void nvs_unlock(void) {
    if (s_nvs_mutex) xSemaphoreGive(s_nvs_mutex);
}

static void save_creds(const char *ssid, const char *pass) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        if (pass && pass[0]) nvs_set_str(h, "pass", pass);
        else nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_unlock();
}

static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return false;
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = ssid_sz;
        if (nvs_get_str(h, "ssid", ssid, &sz) == ESP_OK) {
            sz = pass_sz;
            esp_err_t err = nvs_get_str(h, "pass", pass, &sz);
            if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) ok = true;
        }
        nvs_close(h);
    }
    nvs_unlock();
    return ok;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            fire_evt(WIFI_STA_EVT_DISCONNECTED, data);
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            fire_evt(WIFI_STA_EVT_SCAN_DONE, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xSemaphoreGive(s_got_ip);
        fire_evt(WIFI_STA_EVT_CONNECTED, data);
    }
}

esp_err_t wifi_sta_init(void) {
    if (s_inited) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!s_once) {
        esp_netif_create_default_wifi_sta();
        s_got_ip = xSemaphoreCreateBinary();
        if (!s_got_ip) return ESP_ERR_NO_MEM;
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
        s_once = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_country_t country = { .cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    s_inited = true;
    ESP_LOGI(TAG, "WiFi STA 初始化完成");
    return ESP_OK;
}

int wifi_sta_scan(wifi_ap_info_t *out, int max_count) {
    if (wifi_sta_init() != ESP_OK) return 0;

    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = false;

    ESP_LOGI(TAG, "开始扫描...");
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_cnt = 0;
    esp_wifi_scan_get_ap_num(&ap_cnt);
    if (ap_cnt == 0) { ESP_LOGW(TAG, "未发现 AP"); return 0; }

    uint16_t fetch = ap_cnt < WIFI_SCAN_MAX ? ap_cnt : WIFI_SCAN_MAX;
    wifi_ap_record_t *rec = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!rec) return 0;
    esp_wifi_scan_get_ap_records(&fetch, rec);

    int cnt = 0;
    for (int i = 0; i < fetch && cnt < max_count; i++) {
        strlcpy(out[cnt].ssid, (const char *)rec[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = rec[i].rssi;
        out[cnt].authmode = rec[i].authmode;
        out[cnt].primary = rec[i].primary;
        cnt++;
    }
    free(rec);
    ESP_LOGI(TAG, "扫描完成: %d 个 AP", cnt);
    return cnt;
}

esp_err_t wifi_sta_connect(const char *ssid, const char *pass) {
    if (wifi_sta_init() != ESP_OK) return ESP_ERR_NO_MEM;
    if (s_connected && strcmp(s_cur_ssid, ssid) == 0) return ESP_OK;

    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass && pass[0]) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "连接 %s ...", ssid);

    xSemaphoreTake(s_got_ip, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接启动失败: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(30000)) == pdTRUE) {
        strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
        save_creds(ssid, pass);
        ESP_LOGI(TAG, "已连接 %s", ssid);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连接 %s 超时", ssid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_autoconnect(void) {
    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        ESP_LOGI(TAG, "自动连接: %s", ssid);
        return wifi_sta_connect(ssid, pass[0] ? pass : NULL);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_sta_do_disconnect(void) {
    if (s_connected) { s_connected = false; return esp_wifi_disconnect(); }
    return ESP_OK;
}

bool wifi_sta_is_connected(void) { return s_connected; }
const char *wifi_sta_current_ssid(void) { return s_cur_ssid; }

void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user) {
    s_user_cb = cb; s_user_data = user;
}
void wifi_sta_unregister_cb(wifi_sta_cb_t cb) {
    if (s_user_cb == cb) { s_user_cb = NULL; s_user_data = NULL; }
}

void wifi_sta_stop(void) {
    esp_wifi_stop();
    esp_wifi_deinit();
    s_inited = false;
    s_connected = false;
    ESP_LOGI(TAG, "WiFi STA 已彻底停止");
}

esp_err_t wifi_sta_connect_default(void) {
    return wifi_sta_autoconnect();
}
