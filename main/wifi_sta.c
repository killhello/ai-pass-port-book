// main/wifi_sta.c —— WiFi STA 模块：扫描 + 连接 + NVS 存储 + 事件回调
// 参考 esp32-wifi-manager：事件驱动 + NVS 互斥 + AP 回退 + 幂等初始化
#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>

// strlcpy 兼容层（ESP-IDF 无 strlcpy）
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
static char s_cur_ssid[33] = {0};

static wifi_sta_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

// SmartConfig 状态
static EventGroupHandle_t s_sc_event_group;
static smartconfig_cb_t s_sc_cb = NULL;
static void *s_sc_user_data = NULL;
static volatile bool s_sc_running = false;
static int s_sc_timeout_ms = 60000;

// BLE Provisioning 状态
static EventGroupHandle_t s_bp_event_group;
static ble_prov_cb_t s_bp_cb = NULL;
static void *s_bp_user_data = NULL;
static volatile bool s_bp_running = false;
static int s_bp_timeout_ms = 60000;

static void fire_evt(wifi_sta_evt_t evt, void *data) {
    if (s_user_cb) s_user_cb(evt, data, s_user_data);
}

static void sc_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == SC_EVENT) {
        switch (id) {
            case SC_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "SmartConfig: 扫描完成");
                if (s_sc_cb) s_sc_cb(SC_STATUS_FIND_CHANNEL, s_sc_user_data);
                break;
            case SC_EVENT_FOUND_CHANNEL:
                ESP_LOGI(TAG, "SmartConfig: 找到信道");
                if (s_sc_cb) s_sc_cb(SC_STATUS_GETTING_SSID_PSWD, s_sc_user_data);
                break;
            case SC_EVENT_GOT_SSID_PSWD: {
                ESP_LOGI(TAG, "SmartConfig: 获取到 SSID/Password");
                smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)data;
                wifi_config_t wifi_config = {0};
                memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
                memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
                wifi_config.sta.bssid_set = evt->bssid_set;
                if (wifi_config.sta.bssid_set) {
                    memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
                }
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                ESP_LOGI(TAG, "SmartConfig: SSID=%s, Password=%s", evt->ssid, evt->password);
                if (s_sc_cb) s_sc_cb(SC_STATUS_LINK, s_sc_user_data);
                esp_wifi_connect();
                break;
            }
            case SC_EVENT_SEND_ACK_DONE:
                ESP_LOGI(TAG, "SmartConfig: ACK 发送完成");
                if (s_sc_cb) s_sc_cb(SC_STATUS_LINK_OVER, s_sc_user_data);
                xEventGroupSetBits(s_sc_event_group, BIT1);
                break;
            default:
                break;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "SmartConfig: WiFi 已连接");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "SmartConfig: 获取到 IP");
        xEventGroupSetBits(s_sc_event_group, BIT0);
    }
}

static void smartconfig_task(void *param) {
    (void)param;
    EventBits_t bits = xEventGroupWaitBits(s_sc_event_group, BIT0 | BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(s_sc_timeout_ms));
    esp_smartconfig_stop();
    s_sc_running = false;
    vEventGroupDelete(s_sc_event_group);
    s_sc_event_group = NULL;
    
    if (bits & BIT0) {
        ESP_LOGI(TAG, "SmartConfig 成功，已获取 IP");
        if (s_sc_cb) s_sc_cb(SC_STATUS_LINK_OVER, s_sc_user_data);
    } else if (bits & BIT1) {
        ESP_LOGI(TAG, "SmartConfig: ACK 完成但未获取 IP");
        if (s_sc_cb) s_sc_cb(SC_STATUS_LINK_OVER, s_sc_user_data);
    } else {
        ESP_LOGE(TAG, "SmartConfig 超时或失败");
        if (s_sc_cb) s_sc_cb(SC_STATUS_FAIL, s_sc_user_data);
    }
    vTaskDelete(NULL);
}

static esp_err_t nvs_lock(TickType_t wait) {
    if (!s_nvs_mutex) s_nvs_mutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_nvs_mutex, wait) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void nvs_unlock(void) {
    if (s_nvs_mutex) xSemaphoreGive(s_nvs_mutex);
}

// 保存 SSID+密码到 NVS
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

// 读取保存的 SSID+密码
static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return false;
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = ssid_sz;
        if (nvs_get_str(h, "ssid", ssid, &sz) == ESP_OK) {
            sz = pass_sz;
            esp_err_t err = nvs_get_str(h, "pass", pass, &sz);
            if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
                ok = true;
            }
        }
        nvs_close(h);
    }
    nvs_unlock();
    return ok;
}

// 事件处理
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            fire_evt(WIFI_STA_EVT_DISCONNECTED, data);
            // 不自动重连，交由上层决定
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            fire_evt(WIFI_STA_EVT_SCAN_DONE, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xSemaphoreGive(s_got_ip);
        fire_evt(WIFI_STA_EVT_CONNECTED, data);
    }
}

// 幂等初始化
esp_err_t wifi_sta_init(void) {
    if (s_inited) return ESP_OK;

    s_got_ip = xSemaphoreCreateBinary();
    if (!s_got_ip) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 国家码 CN
    wifi_country_t country = { .cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(500));

    s_inited = true;
    ESP_LOGI(TAG, "WiFi STA 初始化完成");
    return ESP_OK;
}

// 扫描（同步阻塞）
int wifi_sta_scan(wifi_ap_info_t *out, int max_count) {
    if (wifi_sta_init() != ESP_OK) return 0;

    // 使用主动扫描，设置合理的信道停留时间
    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 120;  // 每信道最短 120ms
    cfg.scan_time.active.max = 180;  // 每信道最长 180ms

    ESP_LOGI(TAG, "开始主动扫描...");
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_cnt = 0;
    esp_wifi_scan_get_ap_num(&ap_cnt);
    if (ap_cnt == 0) {
        ESP_LOGW(TAG, "未发现 AP");
        return 0;
    }

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

// 连接（阻塞）
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

    // 等待 IP（30s 超时）
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(30000)) == pdTRUE) {
        strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
        save_creds(ssid, pass);
        ESP_LOGI(TAG, "已连接 %s", ssid);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连接 %s 超时", ssid);
    return ESP_ERR_TIMEOUT;
}

// 自动连接保存的凭证
esp_err_t wifi_sta_autoconnect(void) {
    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        ESP_LOGI(TAG, "自动连接保存的 SSID: %s", ssid);
        return wifi_sta_connect(ssid, pass[0] ? pass : NULL);
    }
    return ESP_ERR_NOT_FOUND;
}

// 断开当前连接
esp_err_t wifi_sta_do_disconnect(void) {
    if (s_connected) {
        s_connected = false;
        return esp_wifi_disconnect();
    }
    return ESP_OK;
}

bool wifi_sta_is_connected(void) { return s_connected; }
const char *wifi_sta_current_ssid(void) { return s_cur_ssid; }

void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user) {
    s_user_cb = cb;
    s_user_data = user;
}
void wifi_sta_unregister_cb(wifi_sta_cb_t cb) {
    if (s_user_cb == cb) { s_user_cb = NULL; s_user_data = NULL; }
}

// AP 回退（可选）
esp_err_t wifi_sta_start_ap_fallback(const char *ap_ssid, const char *ap_pass) {
    if (wifi_sta_init() != ESP_OK) return ESP_ERR_NO_MEM;
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = ap_pass && ap_pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ap_pass && ap_pass[0]) strlcpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password));

    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

// 兼容旧 API：使用 NVS 保存的凭证自动连接
esp_err_t wifi_sta_connect_default(void) {
    return wifi_sta_autoconnect();
}

// ===== SmartConfig 实现 =====
esp_err_t wifi_sta_smartconfig_start(smartconfig_cb_t cb, void *user, int timeout_ms) {
    if (s_sc_running) return ESP_ERR_INVALID_STATE;
    if (wifi_sta_init() != ESP_OK) return ESP_ERR_NO_MEM;

    s_sc_cb = cb;
    s_sc_user_data = user;
    s_sc_timeout_ms = timeout_ms > 0 ? timeout_ms : 60000;

    // 断开现有连接
    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_sc_event_group = xEventGroupCreate();
    if (!s_sc_event_group) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler, NULL);
    if (err != ESP_OK) {
        vEventGroupDelete(s_sc_event_group);
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sc_event_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler);
        vEventGroupDelete(s_sc_event_group);
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sc_event_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sc_event_handler);
        vEventGroupDelete(s_sc_event_group);
        return err;
    }

    smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    sc_cfg.enable_log = true;
    err = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    if (err != ESP_OK) {
        esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sc_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, sc_event_handler);
        vEventGroupDelete(s_sc_event_group);
        return err;
    }

    err = esp_smartconfig_start(&sc_cfg);
    if (err != ESP_OK) {
        esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sc_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, sc_event_handler);
        vEventGroupDelete(s_sc_event_group);
        return err;
    }

    s_sc_running = true;
    BaseType_t ret = xTaskCreate(smartconfig_task, "sc_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        esp_smartconfig_stop();
        esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sc_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, sc_event_handler);
        vEventGroupDelete(s_sc_event_group);
        s_sc_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SmartConfig 启动，等待手机配网...");
    return ESP_OK;
}

void wifi_sta_smartconfig_stop(void) {
    if (!s_sc_running) return;
    esp_smartconfig_stop();
    s_sc_running = false;
    // 事件清理在 smartconfig_task 中完成
}

bool wifi_sta_smartconfig_is_running(void) {
    return s_sc_running;
}

// ===== BLE Provisioning 实现 =====
static void bp_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "BLE Prov: 启动");
                if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_STARTING, s_bp_user_data);
                break;
            case WIFI_PROV_CRED_RECV: {
                ESP_LOGI(TAG, "BLE Prov: 收到凭证");
                wifi_sta_config_t *wifi_cfg = (wifi_sta_config_t *)data;
                ESP_LOGI(TAG, "BLE Prov: SSID=%s, Password=%s", wifi_cfg->ssid, wifi_cfg->password);
                if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_RECEIVING_CREDS, s_bp_user_data);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                ESP_LOGE(TAG, "BLE Prov: 凭证失败");
                if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_FAIL, s_bp_user_data);
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "BLE Prov: 凭证验证成功");
                if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_CONNECTING_WIFI, s_bp_user_data);
                break;
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "BLE Prov: 结束");
                wifi_prov_mgr_deinit();
                xEventGroupSetBits(s_bp_event_group, BIT0);
                break;
            default:
                break;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "BLE Prov: WiFi 已连接");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "BLE Prov: 获取到 IP");
        xEventGroupSetBits(s_bp_event_group, BIT0);
    }
}

static void ble_prov_task(void *param) {
    (void)param;
    EventBits_t bits = xEventGroupWaitBits(s_bp_event_group, BIT0, pdTRUE, pdFALSE, pdMS_TO_TICKS(s_bp_timeout_ms));
    
    if (bits & BIT0) {
        ESP_LOGI(TAG, "BLE Prov 成功，已获取 IP");
        if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_SUCCESS, s_bp_user_data);
    } else {
        ESP_LOGE(TAG, "BLE Prov 超时或失败");
        wifi_prov_mgr_stop_provisioning();
        if (s_bp_cb) s_bp_cb(BLE_PROV_STATUS_FAIL, s_bp_user_data);
    }
    
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, bp_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, bp_event_handler);
    vEventGroupDelete(s_bp_event_group);
    s_bp_event_group = NULL;
    s_bp_running = false;
    vTaskDelete(NULL);
}

// 启动 BLE Provisioning（阻塞直到成功/失败/超时）
esp_err_t wifi_sta_ble_prov_start(ble_prov_cb_t cb, void *user, int timeout_ms) {
    if (s_bp_running) return ESP_ERR_INVALID_STATE;
    if (wifi_sta_init() != ESP_OK) return ESP_ERR_NO_MEM;

    s_bp_cb = cb;
    s_bp_user_data = user;
    s_bp_timeout_ms = timeout_ms > 0 ? timeout_ms : 60000;

    // 断开现有连接
    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_bp_event_group = xEventGroupCreate();
    if (!s_bp_event_group) return ESP_ERR_NO_MEM;

    // 注册事件处理
    esp_err_t err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler, NULL);
    if (err != ESP_OK) {
        vEventGroupDelete(s_bp_event_group);
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, bp_event_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
        vEventGroupDelete(s_bp_event_group);
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, bp_event_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
        vEventGroupDelete(s_bp_event_group);
        return err;
    }

    // 配置 Provisioning Manager
    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };
    
    err = wifi_prov_mgr_init(prov_config);
    if (err != ESP_OK) {
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
        vEventGroupDelete(s_bp_event_group);
        return err;
    }

    // 设置设备名称和服务 UUID
    wifi_prov_scheme_ble_set_service_uuid((uint8_t *)"00001800-0000-1000-8000-00805F9B34FB");
    
    // 启动 Provisioning（设备名为 "ESP32-Prov-XXXX"）
    char service_name[32];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(service_name, sizeof(service_name), "ESP32-Prov-%02X%02X%02X", mac[3], mac[4], mac[5]);
    
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = "12345678";  // Proof of possession (可选)
    
    err = wifi_prov_mgr_start_provisioning(security, pop, service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE Prov 启动失败: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
        vEventGroupDelete(s_bp_event_group);
        return err;
    }

    s_bp_running = true;
    BaseType_t ret = xTaskCreate(ble_prov_task, "bp_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, bp_event_handler);
        vEventGroupDelete(s_bp_event_group);
        s_bp_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE Provisioning 启动，设备名: %s", service_name);
    return ESP_OK;
}

void wifi_sta_ble_prov_stop(void) {
    if (!s_bp_running) return;
    wifi_prov_mgr_stop_provisioning();
    // 事件清理在 ble_prov_task 中完成
}

bool wifi_sta_ble_prov_is_running(void) {
    return s_bp_running;
}