// main/ble_gatt_server.c —— 自定义 BLE GATT 服务（Bluedroid），用于 Web Bluetooth 配网
// UUID 与 ble_provisioning.html 一致
#include "ble_gatt_server.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ble_gatt";

// 自定义服务和特征 UUID (128-bit, 与 HTML 一致)
static const uint8_t svc_uuid_bytes[16] = {
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x00,0x00,0x00,0x18,0x00,0x00
};
static const uint8_t char_ssid_uuid_bytes[16] = {
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x00,0x00,0x00,0x2a,0x00,0x00
};
static const uint8_t char_pass_uuid_bytes[16] = {
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x00,0x00,0x01,0x2a,0x00,0x00
};
static const uint8_t char_ctrl_uuid_bytes[16] = {
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x00,0x00,0x02,0x2a,0x00,0x00
};

#define APP_ID          0x01
#define CHAR_SSID_IDX   0
#define CHAR_PASS_IDX   1
#define CHAR_CTRL_IDX   2
#define CHAR_NUM        3

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static bool s_connected = false;
static uint16_t s_char_handles[CHAR_NUM] = {0};

static char s_received_ssid[33];
static char s_received_pass[65];
static bool s_ssid_received = false;
static bool s_pass_received = false;

static ble_gatt_server_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;
static bool s_inited = false;
static bool s_advertising = false;

static void notify_user(ble_gatt_server_evt_t evt) {
    if (s_user_cb) s_user_cb(evt, s_user_data);
}

// GAP 事件处理
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                s_advertising = true;
                ESP_LOGI(TAG, "BLE 广播已启动");
            } else {
                ESP_LOGE(TAG, "BLE 广播启动失败: %d", param->adv_start_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            s_advertising = false;
            ESP_LOGI(TAG, "BLE 广播已停止");
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "BLE 连接参数更新: status=%d", param->update_conn_params.status);
            break;
        default:
            break;
    }
}

// GATT 事件处理
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                  esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                s_gatts_if = gatts_if;
                ESP_LOGI(TAG, "GATT 应用注册成功, if=%d", gatts_if);
            } else {
                ESP_LOGE(TAG, "GATT 应用注册失败: %d", param->reg.status);
            }
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
            if (param->add_attr_tab.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "属性表创建成功, num=%d", param->add_attr_tab.num_handle);
                for (int i = 0; i < CHAR_NUM && i < param->add_attr_tab.num_handle; i++) {
                    s_char_handles[i] = param->add_attr_tab.handles[i];
                    ESP_LOGI(TAG, "  char[%d] handle=%d", i, s_char_handles[i]);
                }
                // 启动广播
                esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                    .adv_int_min = 0x20,
                    .adv_int_max = 0x40,
                    .adv_type = ADV_TYPE_IND,
                    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                    .channel_map = ADV_CHNL_ALL,
                    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
                });
            } else {
                ESP_LOGE(TAG, "属性表创建失败: %d", param->add_attr_tab.status);
            }
            break;
        }

        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "BLE 已连接, conn_id=%d", param->connect.conn_id);
            s_conn_id = param->connect.conn_id;
            s_connected = true;
            // 停止广播
            esp_ble_gap_stop_advertising();
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "BLE 已断开");
            s_connected = false;
            // 重新广播
            esp_ble_gap_start_advertising(&(esp_ble_adv_params_t){
                .adv_int_min = 0x20,
                .adv_int_max = 0x40,
                .adv_type = ADV_TYPE_IND,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            });
            break;

        case ESP_GATTS_WRITE_EVT: {
            if (!param->write.is_prep) {
                ESP_LOGI(TAG, "GATT 写入: handle=%d, len=%d", param->write.handle, param->write.value_len);
                if (s_char_handles[CHAR_SSID_IDX] && param->write.handle == s_char_handles[CHAR_SSID_IDX]) {
                    int len = param->write.value_len;
                    if (len > 32) len = 32;
                    memcpy(s_received_ssid, param->write.value, len);
                    s_received_ssid[len] = '\0';
                    s_ssid_received = true;
                    ESP_LOGI(TAG, "收到 SSID: %s", s_received_ssid);
                } else if (s_char_handles[CHAR_PASS_IDX] && param->write.handle == s_char_handles[CHAR_PASS_IDX]) {
                    int len = param->write.value_len;
                    if (len > 64) len = 64;
                    memcpy(s_received_pass, param->write.value, len);
                    s_received_pass[len] = '\0';
                    s_pass_received = true;
                    ESP_LOGI(TAG, "收到密码: %s", s_received_pass);
                } else if (s_char_handles[CHAR_CTRL_IDX] && param->write.handle == s_char_handles[CHAR_CTRL_IDX]) {
                    if (param->write.value_len > 0 && param->write.value[0] == 0x01 && s_ssid_received) {
                        ESP_LOGI(TAG, "收到连接命令, SSID=%s", s_received_ssid);
                        notify_user(BLE_GATT_EVT_CREDENTIALS_READY);
                    }
                }
            }
            break;
        }

        case ESP_GATTS_READ_EVT:
            ESP_LOGI(TAG, "GATT 读取: handle=%d", param->read.handle);
            break;

        default:
            break;
    }
}

// 启动广播
static void start_advertising(void) {
    // 广播数据
    static uint8_t adv_data[] = {
        0x02, 0x01, 0x06,        // Flags
        0x03, 0x03, 0x00, 0x18,  // Service UUID (0x1800)
        0x0C, 0x09, 'E', 'S', 'P', '-', 'W', 'i', 'F', 'i',  // Name
    };
    static uint8_t scan_rsp[] = {
        0x0C, 0x09, 'E', 'S', 'P', '-', 'W', 'i', 'F', 'i',  // Scan response name
    };

    esp_ble_gap_config_adv_data_raw(adv_data, sizeof(adv_data));
    esp_ble_gap_config_scan_rsp_data_raw(scan_rsp, sizeof(scan_rsp));
}

esp_err_t ble_gatt_server_init(ble_gatt_server_cb_t cb, void *user) {
    if (s_inited) return ESP_OK;

    s_user_cb = cb;
    s_user_data = user;

    // 释放经典蓝牙内存（ESP32-C3 只支持 BLE）
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器使能失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 使能失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_gap_register_callback(gap_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GAP 回调注册失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_gatts_register_callback(gatts_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GATTS 回调注册失败: %s", esp_err_to_name(err));
        return err;
    }

    // 设置 MTU
    esp_ble_gatt_set_local_mtu(512);

    // 注册 GATT 应用
    err = esp_ble_gatts_app_register(APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GATT 应用注册失败: %s", esp_err_to_name(err));
        return err;
    }

    // 等待注册完成
    vTaskDelay(pdMS_TO_TICKS(500));

    // 创建属性表
    esp_gatt_id_t svc_id = {
        .uuid = { .len = ESP_UUID_LEN_128, .uuid.uuid128 = {0} },
        .inst_id = 0,
    };
    memcpy(svc_id.uuid.uuid128, svc_uuid_bytes, 16);

    esp_gatt_id_t char_uuids[CHAR_NUM];
    memset(char_uuids, 0, sizeof(char_uuids));
    char_uuids[CHAR_SSID_IDX].uuid.len = ESP_UUID_LEN_128;
    memcpy(char_uuids[CHAR_SSID_IDX].uuid.uuid128, char_ssid_uuid_bytes, 16);
    char_uuids[CHAR_PASS_IDX].uuid.len = ESP_UUID_LEN_128;
    memcpy(char_uuids[CHAR_PASS_IDX].uuid.uuid128, char_pass_uuid_bytes, 16);
    char_uuids[CHAR_CTRL_IDX].uuid.len = ESP_UUID_LEN_128;
    memcpy(char_uuids[CHAR_CTRL_IDX].uuid.uuid128, char_ctrl_uuid_bytes, 16);

    // 属性表：1 个服务 + 3 个特征值 + 3 个描述符 = 7
    esp_gatts_attr_db_t attr_tab[CHAR_NUM * 2 + 1];
    memset(attr_tab, 0, sizeof(attr_tab));

    // 服务声明
    attr_tab[0].att_desc.uuid.length = ESP_UUID_LEN_16;
    attr_tab[0].att_desc.uuid.uuid.uuid16 = ESP_GATT_UUID_PRI_SERVICE;
    attr_tab[0].att_desc.perm = ESP_GATT_PERM_READ;
    attr_tab[0].att_desc.value.length = ESP_UUID_LEN_128;
    attr_tab[0].att_desc.value.attr_len = ESP_UUID_LEN_128;
    memcpy(attr_tab[0].att_desc.value.attr_value, svc_uuid_bytes, 16);

    // 特征值声明和值（SSID, PASS, CTRL）
    const uint8_t *char_uuids_bytes[CHAR_NUM] = {
        char_ssid_uuid_bytes, char_pass_uuid_bytes, char_ctrl_uuid_bytes
    };
    for (int i = 0; i < CHAR_NUM; i++) {
        int base = 1 + i * 2;
        // 特征值声明
        attr_tab[base].att_desc.uuid.length = ESP_UUID_LEN_16;
        attr_tab[base].att_desc.uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_DECLARE;
        attr_tab[base].att_desc.perm = ESP_GATT_PERM_READ;
        attr_tab[base].att_desc.value.length = 18;  // 2 + 16
        attr_tab[base].att_desc.value.attr_len = 18;
        uint8_t props = ESP_GATT_CHAR_PROP_BIT_WRITE;
        uint8_t char_val[18];
        char_val[0] = props;
        char_val[1] = 0;  // handle 稍后填
        memcpy(char_val + 2, char_uuids_bytes[i], 16);
        memcpy(attr_tab[base].att_desc.value.attr_value, char_val, 18);

        // 特征值值
        attr_tab[base + 1].att_desc.uuid.length = ESP_UUID_LEN_128;
        memcpy(attr_tab[base + 1].att_desc.uuid.uuid.uuid128, char_uuids_bytes[i], 16);
        attr_tab[base + 1].att_desc.perm = ESP_GATT_PERM_WRITE;
        attr_tab[base + 1].att_desc.value.length = 64;
        attr_tab[base + 1].att_desc.value.attr_len = 0;
    }

    err = esp_ble_gatts_create_attr_tab(attr_tab, s_gatts_if, CHAR_NUM * 2 + 1, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建属性表失败: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "BLE GATT 服务器初始化完成");
    return ESP_OK;
}

void ble_gatt_server_get_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (ssid && ssid_sz > 0) {
        strncpy(ssid, s_received_ssid, ssid_sz - 1);
        ssid[ssid_sz - 1] = '\0';
    }
    if (pass && pass_sz > 0) {
        strncpy(pass, s_received_pass, pass_sz - 1);
        pass[pass_sz - 1] = '\0';
    }
}

void ble_gatt_server_reset(void) {
    s_received_ssid[0] = '\0';
    s_received_pass[0] = '\0';
    s_ssid_received = false;
    s_pass_received = false;
}

void ble_gatt_server_deinit(void) {
    if (!s_inited) return;
    esp_ble_gap_stop_advertising();
    esp_ble_gatts_app_unregister(s_gatts_if);
    s_inited = false;
    s_advertising = false;
    s_gatts_if = ESP_GATT_IF_NONE;
    ESP_LOGI(TAG, "BLE GATT 服务器已停止");
}
