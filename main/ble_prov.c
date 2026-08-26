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
// 特征: 8E7F0002=SSID(写)  8E7F0003=密码(写)  8E7F0004=控制(写, 0x01 触发连接)
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0x7f,0x8e);
static const ble_uuid128_t s_ssid_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x02,0x00,0x7f,0x8e);
static const ble_uuid128_t s_pass_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x03,0x00,0x7f,0x8e);
static const ble_uuid128_t s_ctrl_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x04,0x00,0x7f,0x8e);

static uint16_t s_ssid_val_h, s_pass_val_h, s_ctrl_val_h;
static volatile bool s_running, s_got;
static volatile bool s_have_ssid;           // SSID 已收到, 等密码+控制命令
static char s_ssid[33], s_pass[65];
static uint16_t s_conn_h = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

// GATT 服务表（在 ble_gatts_count_cfg/add_svcs 中注册）
static int cb_ssid(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int cb_pass(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_ssid_uuid.u,
            .access_cb = cb_ssid,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_ssid_val_h },
          { .uuid = &s_pass_uuid.u,
            .access_cb = cb_pass,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_pass_val_h },
          { .uuid = &s_ctrl_uuid.u,
            .access_cb = cb_ctrl,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_ctrl_val_h },
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

// 通用写入: 取 mbuf 全部数据到 buf
static int write_to_buf(struct ble_gatt_access_ctxt *ctxt, char *buf, int bufsz) {
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (uint16_t)bufsz - 1) len = (uint16_t)bufsz - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0)
        return -1;
    buf[len] = 0;
    // 去 CR
    for (char *p = buf; *p; p++) if (*p == 13) memmove(p, p + 1, strlen(p));
    return 0;
}

static int cb_ssid(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char info[48];
        int n = snprintf(info, sizeof(info), "ssid=%s", s_have_ssid ? s_ssid : "-");
        return os_mbuf_append(ctxt->om, info, n);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[40] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (!buf[0]) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    strlcpy(s_ssid, buf, sizeof(s_ssid));
    s_have_ssid = true;
    ESP_LOGI(TAG, "收到 SSID: %s", s_ssid);
    return 0;
}

static int cb_pass(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return os_mbuf_append(ctxt->om, "", 0);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[68] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    strlcpy(s_pass, buf, sizeof(s_pass));
    ESP_LOGI(TAG, "收到密码, 长度=%d", (int)strlen(buf));
    return 0;
}

static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return os_mbuf_append(ctxt->om, "", 0);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[8] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if ((uint8_t)buf[0] != 0x01) {
        ESP_LOGW(TAG, "未知控制命令 0x%02x", (uint8_t)buf[0]);
        return 0;
    }
    if (!s_have_ssid) {
        ESP_LOGW(TAG, "连接命令但未收到 SSID, 忽略");
        return 0;
    }
    ESP_LOGI(TAG, "控制命令: 触发连接");
    s_got = true;
    save_creds(s_ssid, s_pass);
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
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商: %d", event->mtu.value);
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
    s_have_ssid = false;
    s_ssid[0] = 0;
    s_pass[0] = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init 失败: %s", esp_err_to_name(err)); return err; }

    int rc = ble_svc_gap_device_name_set(DEV_NAME);
    if (rc != 0) ESP_LOGE(TAG, "name set 失败: %d", rc);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "count_cfg 失败: %d", rc);
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "add_svcs 失败: %d", rc);

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
