// main/ble_prov.c —— 蓝牙(BLE/NimBLE)配网：GATT 服务器接收 WiFi 凭证
// 手机用 BLE 调试工具(nRF Connect 等)连接后，向配网特征写入两行文本：
//   第1行: WiFi 名称(SSID)
//   第2行: 密码(开放网络留空)
#include "ble_prov.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline size_t strlcpy_local(char *dst, const char *src, size_t dstsize) {
    if (dstsize == 0) return strlen(src);
    size_t n = strlen(src);
    if (n >= dstsize) n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return strlen(src);
}
#define strlcpy(dst, src, dstsize) strlcpy_local(dst, src, dstsize)

static const char *TAG = "ble_prov";
static const char *DEV_NAME = "AI-Passport";
static const char *NVS_NS = "wifi_sta";

// 服务: 8E7F0001-2B4D-4C9A-B5C1-9E3D6F0A5B21
// 特征: 8E7F0002-2B4D-4C9A-B5C1-9E3D6F0A5B21 (可读可写)
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0x7f,0x8e);
static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x02,0x00,0x7f,0x8e);

static uint16_t s_chr_val_h;
static volatile bool s_running, s_got;
static char s_ssid[33], s_pass[65];
static uint16_t s_conn_h = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

// GATT 服务表（在 ble_gatts_count_cfg/add_svcs 中注册）
static int chr_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg);
static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_chr_uuid.u,
            .access_cb = chr_access,
            .arg = NULL,
            .descriptors = NULL,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .min_key_size = 0,
            .val_handle = &s_chr_val_h },
          { 0 } } },
    { 0 }
};
static void adv_restart(void);

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

static int chr_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char info[96];
        int n = snprintf(info, sizeof(info), "ssid=%s", s_ssid[0] ? s_ssid : "-");
        return os_mbuf_append(ctxt->om, info, n);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char buf[112] = {0};
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    buf[len] = 0;

    // 去 CR 后按第一个 LF 分两行: SSID / 密码
    for (char *p = buf; *p; p++) if (*p == 13) memmove(p, p + 1, strlen(p));
    char *nl = strchr(buf, 10);
    const char *pass = "";
    if (nl) { *nl = 0; pass = nl + 1; }
    if (!buf[0]) {
        ESP_LOGW(TAG, "收到空 SSID");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    strlcpy(s_ssid, buf, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    s_got = true;
    save_creds(s_ssid, s_pass);
    ESP_LOGI(TAG, "收到配网凭证 SSID=%s 密码长度=%d", s_ssid, (int)strlen(pass));
    if (s_conn_h != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_h, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_h = event->connect.conn_handle;
            ESP_LOGI(TAG, "手机已连接");
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_h = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "手机断开");
        if (!s_got && s_running) adv_restart();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_got && s_running) adv_restart();
        return 0;
    default:
        return 0;
    }
}

// ---- adv_restart 定义（前向声明在文件头） ----
static void adv_restart(void) {
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)DEV_NAME;
    f.name_len = strlen(DEV_NAME);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields 失败: %d", rc); return; }

    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    r.uuids128 = &s_svc_uuid;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc != 0) { ESP_LOGE(TAG, "adv rsp 失败: %d", rc); return; }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "广播启动失败: %d", rc);
    else ESP_LOGI(TAG, "广播中: %s (堆余 %lu KB)", DEV_NAME,
        (unsigned long)esp_get_free_heap_size() / 1024);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    adv_restart();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE 复位: %d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_prov_start(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_got = false;
    s_ssid[0] = 0;
    s_pass[0] = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init 失败: %s", esp_err_to_name(err)); return err; }

    ble_svc_gap_device_name_set(DEV_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    s_running = true;
    ESP_LOGI(TAG, "BLE 配网启动");
    return ESP_OK;
}

esp_err_t ble_prov_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_conn_h != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_h, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();
    s_conn_h = BLE_HS_CONN_HANDLE_NONE;
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
