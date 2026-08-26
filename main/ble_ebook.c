// main/ble_ebook.c —— BLE 电子书流式传输服务（NimBLE GATT）。
#include "ble_ebook.h"
#include "esp_log.h"
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

static const char *TAG = "ble_ebook";
static const char *DEV_NAME = "AI-Passport";

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0xe8,0xa0);
static const ble_uuid128_t s_filepath_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x10,0x00,0xe8,0xa0);
static const ble_uuid128_t s_pagereq_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x20,0x00,0xe8,0xa0);
static const ble_uuid128_t s_content_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x30,0x00,0xe8,0xa0);
static const ble_uuid128_t s_ctrl_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x40,0x00,0xe8,0xa0);

static uint16_t s_filepath_val_h, s_pagereq_val_h, s_content_val_h, s_ctrl_val_h;
static volatile bool s_running = false;
static volatile bool s_got = false;
static uint16_t s_conn_h = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

static char s_filepath[256];
static char s_filename[128];
static uint32_t s_total_pages = 0;
static uint32_t s_current_page = 0;
static char s_page_buf[1024];
static int s_page_len = 0;

static ble_ebook_state_cb_t s_state_cb = NULL;
static ble_ebook_page_cb_t s_page_cb = NULL;
static ble_ebook_file_cb_t s_file_cb = NULL;
static ble_ebook_state_t s_state = BLE_EBOOK_IDLE;

#define CMD_START       0x01
#define CMD_STOP        0x02
#define CMD_NEXT_PAGE   0x10
#define CMD_PREV_PAGE   0x11
#define CMD_SET_TOTAL   0x30

static inline size_t strlcpy_ebk(char *d, const char *s, size_t n) {
    if (n == 0) return strlen(s);
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l); d[l] = 0;
    return strlen(s);
}

static void extract_filename(const char *path, char *name, size_t nsz) {
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    if (p) p++; else p = path;
    strlcpy_ebk(name, p, nsz);
}

static void set_state(ble_ebook_state_t st) {
    s_state = st;
    if (s_state_cb) s_state_cb(st);
}

static int write_to_buf(struct ble_gatt_access_ctxt *ctxt, char *buf, int bufsz) {
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (uint16_t)bufsz - 1) len = (uint16_t)bufsz - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0) return -1;
    buf[len] = 0;
    for (char *p = buf; *p; p++) if (*p == 13) memmove(p, p + 1, strlen(p));
    return 0;
}

static int cb_filepath(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, s_filepath, strlen(s_filepath));
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (write_to_buf(ctxt, s_filepath, sizeof(s_filepath)) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    extract_filename(s_filepath, s_filename, sizeof(s_filename));
    s_got = true;
    s_current_page = 0;
    ESP_LOGI(TAG, "文件路径: %s", s_filepath);
    if (s_file_cb) s_file_cb(s_filename, s_total_pages);
    set_state(BLE_EBOOK_READY);
    return 0;
}

static int cb_pagereq(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t d[4] = {(uint8_t)(s_current_page),(uint8_t)(s_current_page>>8),
                        (uint8_t)(s_current_page>>16),(uint8_t)(s_current_page>>24)};
        return os_mbuf_append(ctxt->om, d, 4);
    }
    return 0;
}

static int cb_content(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(s_page_buf) - 1) len = sizeof(s_page_buf) - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_page_buf, len, &len) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    s_page_buf[len] = 0;
    s_page_len = len;
    ESP_LOGI(TAG, "第 %lu 页 %d 字节", (unsigned long)s_current_page, s_page_len);
    set_state(BLE_EBOOK_STREAMING);
    if (s_page_cb) s_page_cb(s_page_buf, s_current_page);
    return 0;
}

static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t st = s_running ? 1 : 0;
        return os_mbuf_append(ctxt->om, &st, 1);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t buf[8];
    if (len > 8) len = 8;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint8_t cmd = buf[0];
    switch (cmd) {
    case CMD_START:
        ESP_LOGI(TAG, "开始传输");
        s_current_page = 0;
        set_state(BLE_EBOOK_READY);
        break;
    case CMD_STOP:
        ESP_LOGI(TAG, "停止传输");
        set_state(BLE_EBOOK_CONNECTED);
        break;
    case CMD_NEXT_PAGE:
        s_current_page++;
        ESP_LOGI(TAG, "下一页 %lu", (unsigned long)s_current_page);
        break;
    case CMD_PREV_PAGE:
        if (s_current_page > 0) s_current_page--;
        ESP_LOGI(TAG, "上一页 %lu", (unsigned long)s_current_page);
        break;
    case CMD_SET_TOTAL: {
        if (len >= 5) {
            s_total_pages = buf[1]|(buf[2]<<8)|(buf[3]<<16)|(buf[4]<<24);
            ESP_LOGI(TAG, "总页数 %lu", (unsigned long)s_total_pages);
            if (s_file_cb) s_file_cb(s_filename, s_total_pages);
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "未知命令 0x%02x", cmd);
        break;
    }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_filepath_uuid.u, .access_cb = cb_filepath,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_filepath_val_h },
          { .uuid = &s_pagereq_uuid.u, .access_cb = cb_pagereq,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_pagereq_val_h },
          { .uuid = &s_content_uuid.u, .access_cb = cb_content,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_content_val_h },
          { .uuid = &s_ctrl_uuid.u, .access_cb = cb_ctrl,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_ctrl_val_h },
          { 0 } } },
    { 0 }
};

static void adv_restart(void);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void adv_restart(void) {
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)DEV_NAME;
    f.name_len = strlen(DEV_NAME);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    r.uuids128 = &s_svc_uuid;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    ESP_LOGI(TAG, "广播中: %s", DEV_NAME);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_h = event->connect.conn_handle;
            ESP_LOGI(TAG, "手机已连接");
            set_state(BLE_EBOOK_CONNECTED);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_h = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "手机断开");
        s_got = false;
        set_state(BLE_EBOOK_IDLE);
        if (s_running) adv_restart();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running) adv_restart();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;
    default: return 0;
    }
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    adv_restart();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE 复位 %d", reason); }

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_ebook_start(void) {
    if (s_running) return true;
    s_got = false;
    s_filepath[0] = 0;
    s_filename[0] = 0;
    s_current_page = 0;
    s_total_pages = 0;
    s_page_len = 0;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init 失败 %s", esp_err_to_name(err));
        return false;
    }
    ble_svc_gap_device_name_set(DEV_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    s_running = true;
    set_state(BLE_EBOOK_IDLE);
    ESP_LOGI(TAG, "BLE 电子书启动, 堆 %lu KB",
        (unsigned long)esp_get_free_heap_size() / 1024);
    return true;
}

void ble_ebook_stop(void) {
    if (!s_running) return;
    s_running = false;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_conn_h != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_h, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    nimble_port_stop();
    nimble_port_deinit();
    s_conn_h = BLE_HS_CONN_HANDLE_NONE;
    set_state(BLE_EBOOK_IDLE);
    ESP_LOGI(TAG, "BLE 电子书停止");
}

bool ble_ebook_is_running(void) { return s_running; }
ble_ebook_state_t ble_ebook_get_state(void) { return s_state; }
const char *ble_ebook_get_filename(void) { return s_filename; }
uint32_t ble_ebook_get_current_page(void) { return s_current_page; }
uint32_t ble_ebook_get_total_pages(void) { return s_total_pages; }
const char *ble_ebook_get_page_content(void) { return s_page_buf; }

bool ble_ebook_request_page(uint32_t page_num) {
    if (s_conn_h == BLE_HS_CONN_HANDLE_NONE) return false;
    s_current_page = page_num;
    uint8_t d[4] = {(uint8_t)(page_num),(uint8_t)(page_num>>8),
                    (uint8_t)(page_num>>16),(uint8_t)(page_num>>24)};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, 4);
    if (!om) return false;
    return ble_gattc_notify_custom(s_conn_h, s_pagereq_val_h, om) == 0;
}

void ble_ebook_set_state_cb(ble_ebook_state_cb_t cb) { s_state_cb = cb; }
void ble_ebook_set_page_cb(ble_ebook_page_cb_t cb) { s_page_cb = cb; }
void ble_ebook_set_file_cb(ble_ebook_file_cb_t cb) { s_file_cb = cb; }
