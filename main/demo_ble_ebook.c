// main/demo_ble_ebook.c —— 蓝牙传书页面：接收整本书 + 文件列表管理。
// 按键: UP/DOWN=选择, OK=返回菜单/删除(长按)
#include "demo.h"
#include "ble_ebook.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "demo_ble_ebook";
#define EBOOK_DIR "/spiffs/ebooks"

#define MAX_BOOKS 32
#define NAME_LEN 64

static lv_obj_t *s_scr;
static lv_obj_t *s_title_label;
static lv_obj_t *s_status;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_progress_fill;
static lv_obj_t *s_progress_text;
static lv_obj_t *s_book_list;
static lv_obj_t *s_hint;

// 文件列表
static char s_books[MAX_BOOKS][NAME_LEN];
static int s_book_count = 0;
static int s_sel = 0;

static void scan_books(void) {
    s_book_count = 0;
    s_sel = 0;
    DIR *dir = opendir(EBOOK_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "无法打开 %s", EBOOK_DIR);
        return;
    }
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
    ESP_LOGI(TAG, "找到 %d 本书", s_book_count);
}

static void refresh_book_list(void) {
    if (!s_book_list) return;
    lv_obj_clean(s_book_list);
    if (s_book_count == 0) {
        lv_obj_t *lb = lv_label_create(s_book_list);
        lv_label_set_text(lb, "暂无书籍\n请通过蓝牙传输");
        lv_obj_set_style_text_color(lb, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lb, &notosanssc_16, 0);
        lv_obj_center(lb);
        return;
    }
    for (int i = 0; i < s_book_count; i++) {
        lv_obj_t *lb = lv_label_create(s_book_list);
        lv_label_set_text(lb, s_books[i]);
        lv_obj_set_style_text_font(lb, &notosanssc_16, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(UI_INK), 0);
        lv_obj_set_width(lb, 200);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        if (i == s_sel) {
            lv_obj_set_style_text_color(lb, lv_color_hex(UI_ORANGE), 0);
        }
    }
}

static void update_status(void) {
    if (!s_scr) return;
    ble_ebook_state_t st = ble_ebook_get_state();
    const char *fname = ble_ebook_get_filename();
    uint32_t recvd = ble_ebook_get_received();
    uint32_t total = ble_ebook_get_filesize();

    switch (st) {
    case BLE_EBOOK_IDLE:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_status, "等待连接...");
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hint, "手机搜索 AI-Passport\n连接后选择 TXT 文件发送");
        break;
    case BLE_EBOOK_CONNECTED:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_status, "已连接");
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hint, "等待手机发送文件...");
        break;
    case BLE_EBOOK_RECEIVING: {
        lv_label_set_text(s_title_label, fname);
        char buf[64];
        snprintf(buf, sizeof(buf), "接收中... %lu / %lu",
                 (unsigned long)recvd, (unsigned long)total);
        lv_label_set_text(s_status, buf);
        lv_obj_remove_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        int pct = total > 0 ? (int)((uint64_t)recvd * 100 / total) : 0;
        lv_obj_set_width(s_progress_fill, pct * 2);
        char pt[32];
        snprintf(pt, sizeof(pt), "%d%%", pct);
        lv_label_set_text(s_progress_text, pt);
        lv_label_set_text(s_hint, "");
        break;
    }
    case BLE_EBOOK_DONE:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_status, "传输完成!");
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hint, "按 DOWN 查看书籍列表\n按 OK 返回菜单");
        scan_books();
        refresh_book_list();
        break;
    case BLE_EBOOK_ERROR:
        lv_label_set_text(s_title_label, "蓝牙传书");
        lv_label_set_text(s_status, "传输错误");
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hint, "请重试");
        break;
    }
}

static void on_state_change(ble_ebook_state_t state) {
    if (s_scr && bsp_lvgl_lock(200)) {
        update_status();
        bsp_lvgl_unlock();
    }
}

static void on_progress(uint32_t received, uint32_t total) {
    if (s_scr && bsp_lvgl_lock(100)) {
        update_status();
        bsp_lvgl_unlock();
    }
}

void demo_ble_ebook_enter(void) {
    s_scr = ui_pixel_screen_create("蓝牙传书");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    s_title_label = lv_obj_get_child(panel, 0);
    if (s_title_label) lv_label_set_text(s_title_label, "蓝牙传书");

    s_status = lv_label_create(panel);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_set_style_text_font(s_status, &notosanssc_16, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 8, 4);

    // 进度条
    s_progress_bar = lv_obj_create(panel);
    lv_obj_remove_style_all(s_progress_bar);
    lv_obj_set_size(s_progress_bar, 200, 12);
    lv_obj_align(s_progress_bar, LV_ALIGN_TOP_LEFT, 8, 24);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_bar, 4, 0);
    lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);

    s_progress_fill = lv_obj_create(s_progress_bar);
    lv_obj_remove_style_all(s_progress_fill);
    lv_obj_set_size(s_progress_fill, 0, 12);
    lv_obj_set_pos(s_progress_fill, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fill, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_fill, 4, 0);

    s_progress_text = lv_label_create(s_progress_bar);
    lv_obj_set_style_text_color(s_progress_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_progress_text, &notosanssc_16, 0);
    lv_obj_center(s_progress_text);

    // 书籍列表区
    s_book_list = lv_obj_create(panel);
    lv_obj_remove_style_all(s_book_list);
    lv_obj_set_size(s_book_list, 204, 160);
    lv_obj_align(s_book_list, LV_ALIGN_TOP_LEFT, 8, 40);
    lv_obj_set_scroll_dir(s_book_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_book_list, LV_SCROLLBAR_MODE_OFF);

    // 提示
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

    scan_books();
    refresh_book_list();
    update_status();

    if (!ble_ebook_is_running()) {
        ble_ebook_start();
    }

    ESP_LOGI(TAG, "蓝牙传书页面已启动, 堆 %lu KB",
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
}

void demo_ble_ebook_exit(void) {
    ble_ebook_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = NULL;
        s_progress_bar = NULL;
        s_progress_fill = NULL;
        s_progress_text = NULL;
        s_book_list = NULL;
        s_hint = NULL;
        s_title_label = NULL;
    }
    ESP_LOGI(TAG, "蓝牙传书页面已退出");
}

void demo_ble_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    switch (btn) {
    case BSP_BTN_UP:
        if (s_book_count > 0) {
            s_sel = (s_sel + s_book_count - 1) % s_book_count;
            refresh_book_list();
        }
        break;
    case BSP_BTN_DOWN:
        if (s_book_count > 0) {
            s_sel = (s_sel + 1) % s_book_count;
            refresh_book_list();
        }
        break;
    case BSP_BTN_OK:
        break;
    default:
        break;
    }
}
