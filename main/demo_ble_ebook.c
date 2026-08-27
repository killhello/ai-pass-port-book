// main/demo_ble_ebook.c —— 蓝牙传书页面（精简版，减少内存占用）。
// OK=启动BLE / 返回菜单, UP/DOWN=选择书籍
#include "demo.h"
#include "ble_ebook.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "demo_ble_ebook";
#define EBOOK_DIR "/spiffs/ebooks"
#define MAX_BOOKS 32
#define NAME_LEN 64

static lv_obj_t *s_scr;
static lv_obj_t *s_title_label;
static lv_obj_t *s_info;
static lv_obj_t *s_hint;

static char s_books[MAX_BOOKS][NAME_LEN];
static int s_book_count = 0;
static int s_sel = 0;
static bool s_ble_started = false;

static void scan_books(void) {
    s_book_count = 0;
    DIR *dir = opendir(EBOOK_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_book_count < MAX_BOOKS) {
        if (ent->d_type == DT_REG) {
            const char *name = ent->d_name;
            size_t nlen = strlen(name);
            if (nlen > 4 && strcmp(name + nlen - 4, ".txt") == 0) {
                strlcpy(s_books[s_book_count], name, NAME_LEN);
                s_book_count++;
            }
        }
    }
    closedir(dir);
}

static void update_ui(void) {
    if (!s_scr) return;
    ble_ebook_state_t st = ble_ebook_get_state();
    const char *fname = ble_ebook_get_filename();
    uint32_t recvd = ble_ebook_get_received();
    uint32_t total = ble_ebook_get_filesize();

    switch (st) {
    case BLE_EBOOK_IDLE:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_info, s_ble_started ? "等待连接..." : "按 OK 启动蓝牙");
        lv_label_set_text(s_hint, s_ble_started
            ? "手机搜索 AI-Passport\n连接后选择 TXT 发送"
            : "OK=启动  长按=返回");
        break;
    case BLE_EBOOK_CONNECTED:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_info, "已连接");
        lv_label_set_text(s_hint, "等待手机发送文件...");
        break;
    case BLE_EBOOK_RECEIVING: {
        lv_label_set_text(s_title_label, fname);
        char buf[64];
        int pct = total > 0 ? (int)((uint64_t)recvd * 100 / total) : 0;
        snprintf(buf, sizeof(buf), "接收中 %d%%\n%lu / %lu 字节",
                 pct, (unsigned long)recvd, (unsigned long)total);
        lv_label_set_text(s_info, buf);
        lv_label_set_text(s_hint, "");
        break;
    }
    case BLE_EBOOK_DONE:
        lv_label_set_text(s_title_label, "蓝牙传书");
        scan_books();
        char buf2[128];
        snprintf(buf2, sizeof(buf2), "传输完成!\n共 %d 本书", s_book_count);
        lv_label_set_text(s_info, buf2);
        lv_label_set_text(s_hint, "长按 OK 返回菜单");
        break;
    case BLE_EBOOK_ERROR:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_info, "传输错误");
        lv_label_set_text(s_hint, "长按 OK 返回");
        break;
    }
}

static void on_state_change(ble_ebook_state_t state) {
    if (s_scr && bsp_lvgl_lock(200)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

static void on_progress(uint32_t received, uint32_t total) {
    if (s_scr && bsp_lvgl_lock(100)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

void demo_ble_ebook_enter(void) {
    s_scr = ui_pixel_screen_create("蓝牙传书");

    s_title_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_title_label, &notosanssc_16, 0);
    lv_obj_set_pos(s_title_label, 50, 14);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    s_info = lv_label_create(panel);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_info, &notosanssc_16, 0);
    lv_obj_set_size(s_info, 204, 180);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);

    s_hint = lv_label_create(panel);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_size(s_hint, 204, 40);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_scr);

    ble_ebook_set_state_cb(on_state_change);
    ble_ebook_set_progress_cb(on_progress);

    s_ble_started = false;
    scan_books();
    update_ui();
}

void demo_ble_ebook_exit(void) {
    ble_ebook_stop();
    s_ble_started = false;
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL; s_info = NULL; s_hint = NULL; s_title_label = NULL;
    }
}

void demo_ble_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK && !s_ble_started) {
        if (ble_ebook_start()) {
            s_ble_started = true;
            update_ui();
        }
    }
}
