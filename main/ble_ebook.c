// main/ble_ebook.c —— BLE 整书传输服务（NimBLE GATT）。
// 手机发送文件名+内容，设备存到 SPIFFS /ebooks/ 目录。
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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_ebook";
static const char *DEV_NAME = "AI-Passport";
#define EBOOK_DIR "/spiffs/ebooks"

// UUID: A0E80001 / 0x10=文件名 0x20=内容 0x30=控制
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0xe8,0xa0);
static const ble_uuid128_t s_name_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x10,0x00,0xe8,0xa0);
static const ble_uuid128_t s_data_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x20,0x00,0xe8,0xa0);
static const ble_uuid128_t s_ctrl_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x30,0x00,0xe8,0xa0);

static uint16_t s_name_val_h, s_data_val_h, s_ctrl_val_h;
static volatile bool s_running = false;
static uint16_t s_conn_h = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

static ble_ebook_state_t s_state = BLE_EBOOK_IDLE;
static ble_ebook_state_cb_t s_state_cb = NULL;
static ble_ebook_progress_cb_t s_progress_cb = NULL;

static FILE *s_fp = NULL;
static char s_filename[128];
static uint32_t s_received = 0;
static uint32_t s_filesize = 0;

#define CMD_FILE_START  0x01   // 后跟4字节文件大小
#define CMD_FILE_END    0x02
#define CMD_FILE_ABORT  0x03

static void set_state(ble_ebook_state_t st) {
    s_state = st;
    if (s_state_cb) s_state_cb(st);
}

static inline size_t strlcpy_ebk(char *d, const char *s, size_t n) {
    if (n == 0) return strlen(s);
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l); d[l] = 0;
    return strlen(s);
}

static int write_to_buf(struct ble_gatt_access_ctxt *ctxt, char *buf, int bufsz) {
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (uint16_t)bufsz - 1) len = (uint16_t)bufsz - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0) return -1;
    buf[len] = 0;
    return 0;
}

static void close_file(void) {
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
}

static bool open_file(const char *name) {
    close_file();
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", EBOOK_DIR, name);
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        ESP_LOGE(TAG, "无法创建文件: %s", path);
        return false;
    }
    ESP_LOGI(TAG, "创建文件: %s", path);
    return true;
}

// 特征 0x10: 文件名 (Write)
static int cb_name(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, s_filename, strlen(s_filename));
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[128] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    // 去掉路径前缀，只保留文件名
    const char *p = strrchr(buf, '/');
    if (!p) p = strrchr(buf, '\\');
    if (p) p++; else p = buf;
    strlcpy_ebk(s_filename, p, sizeof(s_filename));
    ESP_LOGI(TAG, "文件名: %s", s_filename);
    return 0;
}

// 特征 0x20: 文件内容 (Write)
static int cb_data(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (!s_fp) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;
    uint8_t buf[512];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    size_t written = fwrite(buf, 1, len, s_fp);
    s_received += written;
    if (s_progress_cb) s_progress_cb(s_received, s_filesize);
    return 0;
}

// 特征 0x30: 控制 (Write)
static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t st = s_state;
        return os_mbuf_append(ctxt->om, &st, 1);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint8_t buf[8];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > 8) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint8_t cmd = buf[0];
    switch (cmd) {
    case CMD_FILE_START:
        s_received = 0;
        s_filesize = 0;
        if (len >= 5) {
            s_filesize = buf[1]|(buf[2]<<8)|(buf[3]<<16)|(buf[4]<<24);
        }
        ESP_LOGI(TAG, "开始接收, 预计 %lu 字节", (unsigned long)s_filesize);
        if (s_filename[0]) {
            if (open_file(s_filename)) {
                set_state(BLE_EBOOK_RECEIVING);
            } else {
                set_state(BLE_EBOOK_ERROR);
            }
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
        // 删除不完整文件
        if (s_filename[0]) {
            char path[192];
            snprintf(path, sizeof(path), "%s/%s", EBOOK_DIR, s_filename);
            remove(path);
        }
        set_state(BLE_EBOOK_IDLE);
        break;
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
          { .uuid = &s_name_uuid.u, .access_cb = cb_name,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_name_val_h },
          { .uuid = &s_data_uuid.u, .access_cb = cb_data,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_data_val_h },
          { .uuid = &s_ctrl_uuid.u, .access_cb = cb_ctrl,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
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
        close_file();
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
    s_received = 0;
    s_filesize = 0;
    s_filename[0] = 0;
    close_file();

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
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    return true;
}

void ble_ebook_stop(void) {
    if (!s_running) return;
    s_running = false;
    close_file();
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
uint32_t ble_ebook_get_received(void) { return s_received; }
uint32_t ble_ebook_get_filesize(void) { return s_filesize; }
void ble_ebook_set_state_cb(ble_ebook_state_cb_t cb) { s_state_cb = cb; }
void ble_ebook_set_progress_cb(ble_ebook_progress_cb_t cb) { s_progress_cb = cb; }
