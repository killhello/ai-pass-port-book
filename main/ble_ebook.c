// main/ble_ebook.c —— BLE 整书传输服务（Bluedroid GATT）。
// 手机发送文件名+内容，设备存到 SPIFFS /ebooks/ 目录。
#include "ble_ebook.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_ebook";
static const char *DEV_NAME = "AI-Passport";
#define EBOOK_DIR "/spiffs/ebooks"
#define APP_ID_EBOOK 0x02

// ---- 128-bit UUID (little-endian) ----
// 服务: A0E80001-2B4D-4C9A-B5C1-9E3D6F0A5B21
static const uint8_t svc_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0xe8,0xa0
};
static const uint8_t name_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x10,0x00,0xe8,0xa0
};
static const uint8_t data_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x20,0x00,0xe8,0xa0
};
static const uint8_t ctrl_uuid[16] = {
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x30,0x00,0xe8,0xa0
};

// ---- 属性表索引 ----
enum {
    IDX_SVC,
    IDX_CHAR_NAME_DECL,
    IDX_CHAR_NAME_VAL,
    IDX_CHAR_DATA_DECL,
    IDX_CHAR_DATA_VAL,
    IDX_CHAR_CTRL_DECL,
    IDX_CHAR_CTRL_VAL,
    EBOOK_IDX_NB
};

static uint16_t handle_table[EBOOK_IDX_NB];
static esp_gatt_if_t s_gatts_if;

#define CMD_FILE_START  0x01
#define CMD_FILE_END    0x02
#define CMD_FILE_ABORT  0x03

static volatile bool s_running = false;
static uint16_t s_conn_id = 0xFFFF;
static volatile ble_ebook_state_t s_state = BLE_EBOOK_IDLE;
static volatile ble_ebook_state_cb_t s_state_cb = NULL;
static volatile ble_ebook_progress_cb_t s_progress_cb = NULL;

static FILE *s_fp = NULL;
static char s_filename[128];
static uint32_t s_received = 0;
static uint32_t s_filesize = 0;

// ---- 标准 UUID ----
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;

// ---- 属性表 ----
static const esp_gatts_attr_db_t gatt_db[EBOOK_IDX_NB] = {
    [IDX_SVC] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16, .uuid_p = (uint8_t *)&primary_service_uuid,
            .perm = ESP_GATT_PERM_READ,
            .max_length = sizeof(svc_uuid), .length = sizeof(svc_uuid), .value = (uint8_t *)svc_uuid
        }
    },
    [IDX_CHAR_NAME_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16, .uuid_p = (uint8_t *)&char_declaration_uuid,
            .perm = ESP_GATT_PERM_READ,
            .max_length = 1, .length = 1,
            .value = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    [IDX_CHAR_NAME_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128, .uuid_p = (uint8_t *)name_uuid,
            .perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            .max_length = 128, .length = 0, .value = NULL
        }
    },
    [IDX_CHAR_DATA_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16, .uuid_p = (uint8_t *)&char_declaration_uuid,
            .perm = ESP_GATT_PERM_READ,
            .max_length = 1, .length = 1,
            .value = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    [IDX_CHAR_DATA_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128, .uuid_p = (uint8_t *)data_uuid,
            .perm = ESP_GATT_PERM_WRITE,
            .max_length = 512, .length = 0, .value = NULL
        }
    },
    [IDX_CHAR_CTRL_DECL] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_16, .uuid_p = (uint8_t *)&char_declaration_uuid,
            .perm = ESP_GATT_PERM_READ,
            .max_length = 1, .length = 1,
            .value = (uint8_t *)(uint8_t[]){ ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE }
        }
    },
    [IDX_CHAR_CTRL_VAL] = {
        .attr_control = { .auto_rsp = ESP_GATT_RSP_BY_APP },
        .att_desc = {
            .uuid_length = ESP_UUID_LEN_128, .uuid_p = (uint8_t *)ctrl_uuid,
            .perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            .max_length = 8, .length = 0, .value = NULL
        }
    },
};

// ---- 状态管理 ----
static void set_state(ble_ebook_state_t st) {
    s_state = st;
    if (s_running && s_state_cb) s_state_cb(st);
}

static void close_file(void) {
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
}

static bool open_file(const char *name) {
    close_file();
    char path[192];
    // 如果文件名包含路径(如 music/xxx.mp3),则保存到 /spiffs/<路径>
    // 否则保存到 /spiffs/ebooks/<文件名>
    if (strchr(name, '/')) {
        snprintf(path, sizeof(path), "/spiffs/%s", name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", EBOOK_DIR, name);
    }
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        ESP_LOGE(TAG, "无法创建文件: %s", path);
        return false;
    }
    ESP_LOGI(TAG, "创建文件: %s", path);
    return true;
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
    default: break;
    }
}

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20, .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL, .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,
    0x11, 0x07,
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0xe8,0xa0,
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
        if (param->reg.status == ESP_GATT_OK) s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(DEV_NAME);
        esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, EBOOK_IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == EBOOK_IDX_NB) {
            memcpy(handle_table, param->add_attr_tab.handles, sizeof(handle_table));
            esp_ble_gatts_start_service(handle_table[IDX_SVC]);
            start_advertising();
        }
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "服务已启动");
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "手机已连接, conn_id=%d", s_conn_id);
        set_state(BLE_EBOOK_CONNECTED);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_conn_id = 0xFFFF;
        ESP_LOGI(TAG, "手机断开");
        close_file();
        if (s_running) { set_state(BLE_EBOOK_IDLE); start_advertising(); }
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU: %d", param->mtu.mtu);
        break;

    case ESP_GATTS_READ_EVT: {
        uint16_t h = param->read.handle;
        if (h == handle_table[IDX_CHAR_NAME_VAL]) {
            esp_ble_gatts_set_attr_value(h, strlen(s_filename), (uint8_t *)s_filename);
            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.len = strlen(s_filename);
            memcpy(rsp.attr_value.value, s_filename, rsp.attr_value.len);
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                         ESP_GATT_OK, &rsp);
        } else if (h == handle_table[IDX_CHAR_CTRL_VAL]) {
            uint8_t st = s_state;
            esp_ble_gatts_set_attr_value(h, 1, &st);
            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.len = 1;
            rsp.attr_value.value[0] = st;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                         ESP_GATT_OK, &rsp);
        }
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.is_prep) break;
        uint16_t h = param->write.handle;
        uint16_t len = param->write.len;
        uint8_t *val = param->write.value;

        if (h == handle_table[IDX_CHAR_NAME_VAL]) {
            // 文件名 (支持路径前缀如 music/xxx.mp3)
            const char *p = (const char *)val;
            // 路径遍历检查
            if (strstr(p, "..")) {
                ESP_LOGW(TAG, "非法文件名: %s", p);
            } else {
                memset(s_filename, 0, sizeof(s_filename));
                size_t n = len < sizeof(s_filename) - 1 ? len : sizeof(s_filename) - 1;
                memcpy(s_filename, p, n);
                s_filename[n] = 0;
                ESP_LOGI(TAG, "文件名: %s", s_filename);
            }
        } else if (h == handle_table[IDX_CHAR_DATA_VAL]) {
            // 文件内容
            if (!s_fp) {
                if (param->write.need_rsp)
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                 param->write.trans_id, ESP_GATT_ERR_UNLIKELY, NULL);
                break;
            }
            size_t written = fwrite(val, 1, len, s_fp);
            if (written != len) {
                ESP_LOGE(TAG, "写入失败: %zu/%u", written, len);
                close_file();
                set_state(BLE_EBOOK_ERROR);
            } else {
                s_received += written;
                if (s_running && s_progress_cb) s_progress_cb(s_received, s_filesize);
            }
        } else if (h == handle_table[IDX_CHAR_CTRL_VAL]) {
            if (len < 1) break;
            uint8_t cmd = val[0];
            switch (cmd) {
            case CMD_FILE_START:
                s_received = 0;
                s_filesize = 0;
                if (len >= 5) {
                    s_filesize = val[1]|(val[2]<<8)|(val[3]<<16)|(val[4]<<24);
                }
                ESP_LOGI(TAG, "开始接收, 预计 %lu 字节", (unsigned long)s_filesize);
                if (s_filename[0]) {
                    if (open_file(s_filename)) set_state(BLE_EBOOK_RECEIVING);
                    else set_state(BLE_EBOOK_ERROR);
                }
                break;
            case CMD_FILE_END:
                ESP_LOGI(TAG, "传输完成, 共 %lu 字节", (unsigned long)s_received);
                close_file();
                set_state(BLE_EBOOK_DONE);
                break;
            case CMD_FILE_ABORT:
                ESP_LOGI(TAG, "传输中止");
                close_file();
                if (s_filename[0]) {
                    char path[192];
                    if (strchr(s_filename, '/'))
                        snprintf(path, sizeof(path), "/spiffs/%s", s_filename);
                    else
                        snprintf(path, sizeof(path), "%s/%s", EBOOK_DIR, s_filename);
                    remove(path);
                }
                set_state(BLE_EBOOK_IDLE);
                break;
            default:
                ESP_LOGW(TAG, "未知命令 0x%02x", cmd);
                break;
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
bool ble_ebook_start(void) {
    if (s_running) return true;
    s_received = 0;
    s_filesize = 0;
    s_filename[0] = 0;
    close_file();

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "释放 Classic BT 内存: %s", esp_err_to_name(err));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err)); return false; }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT 控制器使能失败: %s", esp_err_to_name(err)); return false; }

    err = esp_bluedroid_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(err)); return false; }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Bluedroid 使能失败: %s", esp_err_to_name(err)); return false; }

    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatt_set_local_mtu(512);

    esp_ble_gatts_app_register(APP_ID_EBOOK);

    s_running = true;
    set_state(BLE_EBOOK_IDLE);
    ESP_LOGI(TAG, "BLE 电子书启动, 堆 %lu KB",
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    return true;
}

void ble_ebook_stop(void) {
    if (!s_running) return;
    s_running = false;
    close_file();
    esp_ble_gap_stop_advertising();
    s_conn_id = 0xFFFF;
    s_state = BLE_EBOOK_IDLE;
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "BLE 电子书停止");
}

bool ble_ebook_is_running(void) { return s_running; }
ble_ebook_state_t ble_ebook_get_state(void) { return s_state; }
const char *ble_ebook_get_filename(void) { return s_filename; }
uint32_t ble_ebook_get_received(void) { return s_received; }
uint32_t ble_ebook_get_filesize(void) { return s_filesize; }
void ble_ebook_set_state_cb(ble_ebook_state_cb_t cb) { s_state_cb = cb; }
void ble_ebook_set_progress_cb(ble_ebook_progress_cb_t cb) { s_progress_cb = cb; }
