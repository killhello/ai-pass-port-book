// main/ble_prov.c —— 蓝牙(Bluedroid BLE)配网：GATT 服务器接收 WiFi 凭证
// 手机用 BLE 调试工具(nRF Connect 等)连接后，向配网特征写入两行文本：
//   第1行: WiFi 名称(SSID)
//   第2行: 密码(开放网络留空)
#include "ble_prov.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_prov";
static const char *DEV_NAME = "AI-Passport";
static const char *NVS_NS = "wifi_sta";

#define APP_ID_PROV 0x01

// ---- 128-bit UUID (little-endian) ----
// 服务: 8E7F0001-2B4D-4C9A-B5C1-9E3D6F0A5B21
static const uint8_t svc_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0x7f,0x8e
};
// 特征: SSID=..02, PASS=..03, CTRL=..04
static const uint8_t ssid_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x02,0x00,0x7f,0x8e
};
static const uint8_t pass_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x03,0x00,0x7f,0x8e
};
static const uint8_t ctrl_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x04,0x00,0x7f,0x8e
};

// ---- 属性表索引 ----
enum {
    IDX_SVC,
    IDX_CHAR_SSID_DECL,
    IDX_CHAR_SSID_VAL,
    IDX_CHAR_PASS_DECL,
    IDX_CHAR_PASS_VAL,
    IDX_CHAR_CTRL_DECL,
    IDX_CHAR_CTRL_VAL,
    PROV_IDX_NB
};

static uint16_t handle_table[PROV_IDX_NB];
static esp_gatt_if_t s_gatts_if;

static volatile bool s_running, s_got;
static volatile bool s_have_ssid;
static char s_ssid[33], s_pass[65];
static uint16_t s_conn_id = 0xFFFF;

// ---- 标准 UUID 常量 ----
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;

// ---- 属性表 ----
static const esp_gatts_attr_db_t gatt_db[PROV_IDX_NB] = {
    // Service Declaration
    [IDX_SVC] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&primary_service_uuid,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = sizeof(svc_uuid),
            .length      = sizeof(svc_uuid),
            .value       = (uint8_t *)svc_uuid
        }
    },
    // SSID Characteristic Declaration
    [IDX_CHAR_SSID_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_declaration_uuid,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = 1,
            .length      = 1,
            .value       = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    // SSID Characteristic Value
    [IDX_CHAR_SSID_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = (uint8_t *)ssid_uuid,
            .perm        = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            .max_length  = 64,
            .length      = 0,
            .value       = NULL
        }
    },
    // Password Characteristic Declaration
    [IDX_CHAR_PASS_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_declaration_uuid,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = 1,
            .length      = 1,
            .value       = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    // Password Characteristic Value
    [IDX_CHAR_PASS_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = (uint8_t *)pass_uuid,
            .perm        = ESP_GATT_PERM_WRITE,
            .max_length  = 128,
            .length      = 0,
            .value       = NULL
        }
    },
    // Control Characteristic Declaration
    [IDX_CHAR_CTRL_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16,
            .uuid_p      = (uint8_t *)&char_declaration_uuid,
            .perm        = ESP_GATT_PERM_READ,
            .max_length  = 1,
            .length      = 1,
            .value       = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    // Control Characteristic Value
    [IDX_CHAR_CTRL_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128,
            .uuid_p      = (uint8_t *)ctrl_uuid,
            .perm        = ESP_GATT_PERM_WRITE,
            .max_length  = 8,
            .length      = 0,
            .value       = NULL
        }
    },
};

// ---- NVS 保存 ----
static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        if (pass[0]) nvs_set_str(h, "pass", pass);
        else nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "凭证已存 NVS: %s", ssid);
    }
}

// ---- GAP 事件 ----
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            ESP_LOGE(TAG, "广播启动失败: %d", param->adv_start_cmpl.status);
        else
            ESP_LOGI(TAG, "广播中: %s (堆余 %lu KB)", DEV_NAME,
                (unsigned long)esp_get_free_heap_size() / 1024);
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "广播已停");
        break;
    default: break;
    }
}

// ---- 广播参数 ----
static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,
    0x11, 0x07,
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0x7f,0x8e,
    0x0C, 0x09,
    'A','I','-','P','a','s','s','p','o','r','t'
};

static void start_advertising(void) {
    esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
    esp_ble_gap_start_advertising(&adv_params);
}

// ---- GATTS 事件 ----
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            s_gatts_if = gatts_if;
        }
        esp_ble_gap_set_device_name(DEV_NAME);
        esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, PROV_IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == PROV_IDX_NB) {
            memcpy(handle_table, param->add_attr_tab.handles, sizeof(handle_table));
            esp_ble_gatts_start_service(handle_table[IDX_SVC]);
            start_advertising();
        } else {
            ESP_LOGE(TAG, "属性表创建失败: %d", param->add_attr_tab.status);
        }
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "服务已启动");
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "手机已连接, conn_id=%d", s_conn_id);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_conn_id = 0xFFFF;
        ESP_LOGI(TAG, "手机断开, reason=0x%x", param->disconnect.reason);
        if (s_running && !s_got) start_advertising();
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU: %d", param->mtu.mtu);
        break;

    case ESP_GATTS_READ_EVT: {
        uint16_t h = param->read.handle;
        if (h == handle_table[IDX_CHAR_SSID_VAL]) {
            char info[48];
            int n = snprintf(info, sizeof(info), "ssid=%s", s_have_ssid ? s_ssid : "-");
            esp_attr_value_t rsp = { .attr_max_len = sizeof(info), .attr_len = n, .attr_value = (uint8_t *)info };
            esp_ble_gatts_set_attr_value(h, n, (uint8_t *)info);
            esp_gatt_rsp_t g_rsp;
            memset(&g_rsp, 0, sizeof(g_rsp));
            g_rsp.attr_value.len = n;
            memcpy(g_rsp.attr_value.value, info, n);
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                         ESP_GATT_OK, &g_rsp);
        }
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.is_prep) break;
        uint16_t h = param->write.handle;
        uint16_t len = param->write.value_len;
        uint8_t *val = param->write.value;

        if (h == handle_table[IDX_CHAR_SSID_VAL]) {
            if (len > 0 && len < sizeof(s_ssid)) {
                memcpy(s_ssid, val, len);
                s_ssid[len] = 0;
                s_have_ssid = true;
                ESP_LOGI(TAG, "收到 SSID: %s", s_ssid);
            }
        } else if (h == handle_table[IDX_CHAR_PASS_VAL]) {
            if (len < sizeof(s_pass)) {
                memcpy(s_pass, val, len);
                s_pass[len] = 0;
                ESP_LOGI(TAG, "收到密码, 长度=%d", len);
            }
        } else if (h == handle_table[IDX_CHAR_CTRL_VAL]) {
            if (len >= 1 && val[0] == 0x01) {
                if (!s_have_ssid) {
                    ESP_LOGW(TAG, "连接命令但未收到 SSID, 忽略");
                } else {
                    ESP_LOGI(TAG, "控制命令: 触发连接");
                    s_got = true;
                    save_creds(s_ssid, s_pass);
                    if (s_conn_id != 0xFFFF)
                        esp_ble_gap_disconnect(param->write.remote_bda);
                }
            }
        }
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                         param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
    }

    default: break;
    }
}

// ---- 公共 API ----
esp_err_t ble_prov_start(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_got = false;
    s_have_ssid = false;
    s_ssid[0] = 0;
    s_pass[0] = 0;

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "释放 Classic BT 内存: %s", esp_err_to_name(err));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err)); return err; }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT 控制器使能失败: %s", esp_err_to_name(err)); return err; }

    err = esp_bluedroid_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(err)); return err; }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Bluedroid 使能失败: %s", esp_err_to_name(err)); return err; }

    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatt_set_local_mtu(512);

    esp_ble_gatts_app_register(APP_ID_PROV);

    s_running = true;
    ESP_LOGI(TAG, "BLE 配网启动");
    return ESP_OK;
}

esp_err_t ble_prov_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;
    esp_ble_gap_stop_advertising();
    if (s_conn_id != 0xFFFF) {
        // 不主动断开，避免死锁
        s_conn_id = 0xFFFF;
    }
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "BLE 配网停止, 堆余 %lu KB",
        (unsigned long)esp_get_free_heap_size() / 1024);
    return ESP_OK;
}

bool ble_prov_is_running(void) { return s_running; }

bool ble_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (!s_got) return false;
    strlcpy(ssid, s_ssid, ssid_sz);
    strlcpy(pass, s_pass, pass_sz);
    s_got = false;
    return true;
}
