// main/ble_gatt_server.c —— 自定义 BLE GATT 服务，用于 Web Bluetooth 配网
// UUID 与 ble_provisioning.html 一致
#include "ble_gatt_server.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_gap.h"
#include "services/gatt/ble_gatt_svc.h"
#include <string.h>

static const char *TAG = "ble_gatt";

// 自定义服务和特征 UUID (与 HTML 一致)
#define SVC_UUID_BASE128  0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x00,0x18,0x00,0x00
#define CHAR_SSID_UUID128 0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x00,0x2a,0x00,0x00
#define CHAR_PASS_UUID128 0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x01,0x2a,0x00,0x00
#define CHAR_CTRL_UUID128 0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x02,0x2a,0x00,0x00

static ble_uuid_svc_t svc_uuid;
static ble_uuid128_t char_ssid_uuid;
static ble_uuid128_t char_pass_uuid;
static ble_uuid128_t char_ctrl_uuid;

static int svc_handle;
static int char_ssid_handle;
static int char_pass_handle;
static int ctrl_handle;

static char s_received_ssid[33];
static char s_received_pass[65];
static bool s_ssid_received = false;
static bool s_pass_received = false;

static ble_gatt_server_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

static void notify_user(ble_gatt_server_evt_t evt) {
    if (s_user_cb) s_user_cb(evt, s_user_data);
}

// GAP 事件处理
static int on_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->status == 0) {
                ESP_LOGI(TAG, "BLE 已连接, conn_id=%d", event->connect.conn_handle);
            } else {
                ESP_LOGE(TAG, "BLE 连接失败: %d", event->status);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE 断开");
            // 重新开始广播
            ble_gatt_server_start_advertising();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "BLE 订阅: attr_handle=%d", event->subscribe.attr_handle);
            break;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "BLE MTU 更新: %d", event->mtu.value);
            break;
        default:
            break;
    }
    return 0;
}

// GATT 事件处理（特征写入）
static int on_gatt_event(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)arg;
    (void)conn_handle;

    if (attr_handle == char_ssid_handle) {
        // SSID 写入
        int len = ctxt->om->om_len;
        if (len > 32) len = 32;
        memcpy(s_received_ssid, ctxt->om->om_data, len);
        s_received_ssid[len] = '\0';
        s_ssid_received = true;
        ESP_LOGI(TAG, "收到 SSID: %s", s_received_ssid);
        return 0;
    }
    if (attr_handle == char_pass_handle) {
        // 密码写入
        int len = ctxt->om->om_len;
        if (len > 64) len = 64;
        memcpy(s_received_pass, ctxt->om->om_data, len);
        s_received_pass[len] = '\0';
        s_pass_received = true;
        ESP_LOGI(TAG, "收到密码: %s", s_received_pass);
        return 0;
    }
    if (attr_handle == ctrl_handle) {
        // 控制命令写入
        if (ctxt->om->om_len > 0) {
            uint8_t cmd = ctxt->om->om_data[0];
            if (cmd == 0x01 && s_ssid_received) {
                ESP_LOGI(TAG, "收到连接命令, SSID=%s", s_received_ssid);
                notify_user(BLE_GATT_EVT_CREDENTIALS_READY);
            }
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// GATT 服务定义
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &char_ssid_uuid.u,
                .access_cb = on_gatt_event,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &char_pass_uuid.u,
                .access_cb = on_gatt_event,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &char_ctrl_uuid.u,
                .access_cb = on_gatt_event,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0},  // 结束
        },
    },
    {0},  // 结束
};

// 广播数据
static void ble_gatt_server_start_advertising(void) {
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;  // 20ms
    adv_params.itvl_max = 0x40;  // 40ms

    // 广播数据：设备名 + 服务 UUID
    uint8_t adv_data[] = {
        0x02, 0x01, 0x06,                    // Flags
        0x03, 0x03, 0x00, 0x18,              // UUID 列表 (0x1800)
        0x07, 0x09, 'E', 'S', 'P', '-', 'W', 'i', 'F', 'i',  // 名称
    };
    uint8_t rsp_data[] = {
        0x07, 0x09, 'E', 'S', 'P', '-', 'W', 'i', 'F', 'i',  // 扫描响应名称
    };

    ble_gap_set_adv_data(sizeof(adv_data), adv_data);
    ble_gap_set_scan_rsp_data(sizeof(rsp_data), rsp_data);
    ble_gap_adv_start(ble_hs_id_infer_auto(0, NULL), NULL, BLE_HS_FOREVER, &adv_params, on_gap_event, NULL);
}

// Host 任务回调
static void on_host_sync(void) {
    ESP_LOGI(TAG, "BLE host 已同步");
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, NULL);

    // 注册 GATT 服务
    ble_gatts_count_handles(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    // 获取特征值句柄
    ble_gatts_find_svc(&svc_uuid.u, &svc_handle);
    ble_gatts_find_chr(&svc_uuid.u, &char_ssid_uuid.u, NULL, &char_ssid_handle);
    ble_gatts_find_chr(&svc_uuid.u, &char_pass_uuid.u, NULL, &char_pass_handle);
    ble_gatts_find_chr(&svc_uuid.u, &char_ctrl_uuid.u, NULL, &ctrl_handle);

    ble_gatt_server_start_advertising();
    ESP_LOGI(TAG, "BLE GATT 服务已启动，等待连接...");
}

// Host 任务
static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    vTaskDelete(NULL);
}

esp_err_t ble_gatt_server_init(ble_gatt_server_cb_t cb, void *user) {
    s_user_cb = cb;
    s_user_data = user;

    // 初始化 UUID
    ble_uuid128_init(&char_ssid_uuid, CHAR_SSID_UUID128);
    ble_uuid128_init(&char_pass_uuid, CHAR_PASS_UUID128);
    ble_uuid128_init(&char_ctrl_uuid, CHAR_CTRL_UUID128);
    ble_uuid128_init(&svc_uuid, SVC_UUID_BASE128);

    // NimBLE 初始化
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.sync_cb = on_host_sync;

    // 启动 Host 任务
    BaseType_t ret = xTaskCreate(ble_host_task, "nimble_host", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "BLE Host 任务创建失败");
        return ESP_ERR_NO_MEM;
    }

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
    ble_gap_adv_stop();
    ble_hs_cfg.sync_cb = NULL;
    ESP_LOGI(TAG, "BLE GATT 服务器已停止");
}
