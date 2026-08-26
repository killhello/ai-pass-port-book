// main/demo_ble_ebook.c —— 蓝牙传书页面。
// 按键: UP=上一页, DOWN=下一页, OK=返回菜单
// 手机通过 BLE 发送文件路径和页面内容
#include "demo.h"
#include "ble_ebook.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "demo_ble_ebook";

static lv_obj_t *s_scr;
static lv_obj_t *s_status;      // 状态文本
static lv_obj_t *s_filename;    // 文件名
static lv_obj_t *s_page_info;   // 页码信息
static lv_obj_t *s_text;        // 正文显示
static lv_obj_t *s_hint;        // 提示文本

static ble_ebook_state_t s_last_state = BLE_EBOOK_IDLE;

static void update_ui(void) {
    if (!s_scr) return;

    ble_ebook_state_t state = ble_ebook_get_state();
    const char *fname = ble_ebook_get_filename();
    uint32_t page = ble_ebook_get_current_page();
    uint32_t total = ble_ebook_get_total_pages();
    const char *content = ble_ebook_get_page_content();

    switch (state) {
    case BLE_EBOOK_IDLE:
        lv_label_set_text(s_status, "等待手机连接...");
        lv_label_set_text(s_filename, "");
        lv_label_set_text(s_page_info, "");
        lv_label_set_text(s_text, "");
        lv_label_set_text(s_hint, "手机搜索 AI-Passport\n连接后选择 TXT 文件");
        break;
    case BLE_EBOOK_CONNECTED:
        lv_label_set_text(s_status, "已连接");
        lv_label_set_text(s_filename, fname[0] ? fname : "等待发送文件...");
        lv_label_set_text(s_page_info, "");
        lv_label_set_text(s_text, "");
        lv_label_set_text(s_hint, "请在手机上选择\nTXT 电子书文件");
        break;
    case BLE_EBOOK_READY:
        lv_label_set_text(s_status, "传输中");
        lv_label_set_text(s_filename, fname);
        if (total > 0) {
            char info[32];
            snprintf(info, sizeof(info), "%lu / %lu",
                     (unsigned long)(page + 1), (unsigned long)total);
            lv_label_set_text(s_page_info, info);
        } else {
            lv_label_set_text(s_page_info, "等待内容...");
        }
        lv_label_set_text(s_text, content[0] ? content : "");
        lv_label_set_text(s_hint, content[0] ? "" : "等待手机发送\n页面内容...");
        break;
    case BLE_EBOOK_STREAMING:
        lv_label_set_text(s_status, "传输中");
        lv_label_set_text(s_filename, fname);
        if (total > 0) {
            char info[32];
            snprintf(info, sizeof(info), "%lu / %lu",
                     (unsigned long)(page + 1), (unsigned long)total);
            lv_label_set_text(s_page_info, info);
        }
        lv_label_set_text(s_text, content);
        lv_label_set_text(s_hint, "");
        break;
    }
}

static void on_state_change(ble_ebook_state_t state) {
    s_last_state = state;
    if (s_scr) {
        if (bsp_lvgl_lock(200)) {
            update_ui();
            bsp_lvgl_unlock();
        }
    }
}

static void on_page_received(const char *content, uint32_t page_num) {
    ESP_LOGI(TAG, "收到第 %lu 页", (unsigned long)page_num);
    if (s_scr && bsp_lvgl_lock(200)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

static void on_file_info(const char *filename, uint32_t total_pages) {
    ESP_LOGI(TAG, "文件: %s, %lu 页", filename, (unsigned long)total_pages);
    if (s_scr && bsp_lvgl_lock(200)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

void demo_ble_ebook_enter(void) {
    s_scr = ui_pixel_screen_create("蓝牙传书");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    s_status = lv_label_create(panel);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_set_style_text_font(s_status, &notosanssc_16, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 4);

    s_filename = lv_label_create(panel);
    lv_obj_set_style_text_color(s_filename, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_filename, &notosanssc_16, 0);
    lv_obj_align(s_filename, LV_ALIGN_TOP_LEFT, 8, 24);
    lv_obj_set_size(s_filename, 200, 20);
    lv_label_set_long_mode(s_filename, LV_LABEL_LONG_DOT);

    s_page_info = lv_label_create(panel);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_text_font(s_page_info, &notosanssc_16, 0);
    lv_obj_align(s_page_info, LV_ALIGN_TOP_RIGHT, -8, 24);

    s_text = lv_label_create(panel);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_text, &notosanssc_16, 0);
    lv_obj_set_size(s_text, 204, 180);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 8, 48);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);

    s_hint = lv_label_create(panel);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_size(s_hint, 204, 100);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 80);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_scr);

    // 注册回调
    ble_ebook_set_state_cb(on_state_change);
    ble_ebook_set_page_cb(on_page_received);
    ble_ebook_set_file_cb(on_file_info);

    // 启动 BLE 服务
    if (!ble_ebook_is_running()) {
        ble_ebook_start();
    }

    update_ui();
    ESP_LOGI(TAG, "蓝牙传书页面已启动");
}

void demo_ble_ebook_exit(void) {
    ble_ebook_stop();

    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = NULL;
        s_filename = NULL;
        s_page_info = NULL;
        s_text = NULL;
        s_hint = NULL;
    }

    ESP_LOGI(TAG, "蓝牙传书页面已退出");
}

void demo_ble_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    ble_ebook_state_t state = ble_ebook_get_state();

    switch (btn) {
    case BSP_BTN_UP:
        if (state == BLE_EBOOK_STREAMING || state == BLE_EBOOK_READY) {
            uint32_t page = ble_ebook_get_current_page();
            if (page > 0) {
                ble_ebook_request_page(page - 1);
                update_ui();
            }
        }
        break;
    case BSP_BTN_DOWN:
        if (state == BLE_EBOOK_STREAMING || state == BLE_EBOOK_READY) {
            uint32_t page = ble_ebook_get_current_page();
            ble_ebook_request_page(page + 1);
            update_ui();
        }
        break;
    case BSP_BTN_OK:
        break;
    default:
        break;
    }
}
