// main/demo_ebook.c —— 电子书阅读器页面（单 panel 方案，避免阴影遮挡）。
// 菜单: UP/DOWN=选择书籍, OK=阅读, 长按 OK=返回
// 阅读: UP=上一页, DOWN=下一页, OK=息屏
#include "demo.h"
#include "ebook_reader.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_ebook";

typedef enum { VIEW_LIST, VIEW_READ } view_mode_t;

static view_mode_t s_mode = VIEW_LIST;
static lv_obj_t *s_scr;
static lv_obj_t *s_title_label;

static lv_obj_t *s_panel;
static lv_obj_t *s_hint;

static lv_obj_t *s_list_items[8];
static ebook_book_t s_books[EBOOK_MAX_BOOKS];
static int s_book_count = 0;
static int s_book_sel = 0;
static int s_book_scroll = 0;

static lv_obj_t *s_text;
static lv_obj_t *s_page_info;
static lv_obj_t *s_read_hint;

static ebook_reader_t s_reader;
static bool s_screen_off = false;

static void refresh_list(void) {
    if (!s_panel) return;
    for (int i = 0; i < 8; i++) {
        if (s_list_items[i]) { lv_obj_delete(s_list_items[i]); s_list_items[i] = NULL; }
    }
    int vis = s_book_count - s_book_scroll;
    if (vis > 8) vis = 8;
    if (vis < 0) vis = 0;
    for (int i = 0; i < vis; i++) {
        int idx = s_book_scroll + i;
        lv_obj_t *lb = lv_label_create(s_panel);
        lv_obj_set_style_text_font(lb, &notosanssc_16, 0);
        lv_label_set_text(lb, s_books[idx].name);
        lv_obj_set_width(lb, 200);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(lb, 4, i * 22);
        lv_obj_set_style_text_color(lb,
            idx == s_book_sel ? lv_color_hex(UI_ORANGE) : lv_color_hex(UI_INK), 0);
        s_list_items[i] = lb;
    }
    if (s_book_count == 0) {
        lv_obj_t *lb = lv_label_create(s_panel);
        lv_label_set_text(lb, "暂无书籍");
        lv_obj_set_style_text_color(lb, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lb, &notosanssc_16, 0);
        lv_obj_center(lb);
        s_list_items[0] = lb;
    }
}

static void show_list(void) {
    s_mode = VIEW_LIST;
    if (s_text) lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
    if (s_page_info) lv_obj_add_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);
    if (s_read_hint) lv_obj_add_flag(s_read_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title_label, "电子书");
    lv_label_set_text(s_hint, s_book_count > 0
        ? "UP/DOWN 选择 OK 阅读\n长按 OK 返回" : "暂无书籍\n请通过蓝牙传书");
    refresh_list();
}

static void show_read(void) {
    s_mode = VIEW_READ;
    for (int i = 0; i < 8; i++) {
        if (s_list_items[i]) { lv_obj_delete(s_list_items[i]); s_list_items[i] = NULL; }
    }
    if (s_text) lv_obj_remove_flag(s_text, LV_OBJ_FLAG_HIDDEN);
    if (s_page_info) lv_obj_remove_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);
    if (s_read_hint) lv_obj_remove_flag(s_read_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title_label, s_books[s_book_sel].name);
    lv_label_set_text(s_hint, "UP=上页 DOWN=下页\nOK=息屏 长按OK=返回");
}

static void update_page_display(void) {
    if (!s_reader.is_open) return;
    lv_label_set_text(s_text, s_reader.page_buf);
    char info[32];
    snprintf(info, sizeof(info), "%lu / %lu",
             (unsigned long)(s_reader.current_page + 1),
             (unsigned long)s_reader.total_pages);
    lv_label_set_text(s_page_info, info);
}

void demo_ebook_enter(void) {
    s_screen_off = false;
    s_scr = ui_pixel_screen_create("电子书");

    s_title_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_title_label, &notosanssc_16, 0);
    lv_obj_set_pos(s_title_label, 50, 14);
    lv_obj_set_width(s_title_label, 185);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);

    s_panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    s_text = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_size(s_text, 204, 190);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_text, &notosanssc_16, 0);
    lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);

    s_page_info = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_ORANGE), 0);
    lv_obj_set_style_text_font(s_page_info, &notosanssc_16, 0);
    lv_obj_align(s_page_info, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);

    s_read_hint = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_read_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_read_hint, &notosanssc_16, 0);
    lv_obj_align(s_read_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(s_read_hint, LV_OBJ_FLAG_HIDDEN);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_size(s_hint, 220, 30);

    lv_screen_load(s_scr);

    ebook_reader_init(&s_reader);
    s_book_count = ebook_reader_scan_books(s_books, EBOOK_MAX_BOOKS);
    s_book_sel = 0;
    s_book_scroll = 0;

    show_list();
    ESP_LOGI(TAG, "电子书页面已启动, %d 本书", s_book_count);
}

void demo_ebook_exit(void) {
    if (s_reader.is_open) {
        ebook_reader_save_progress(&s_reader);
        ebook_reader_close(&s_reader);
    }
    if (s_screen_off) bsp_display_backlight(100);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL; s_text = NULL; s_page_info = NULL;
        s_title_label = NULL; s_panel = NULL;
        s_hint = NULL; s_read_hint = NULL;
        memset(s_list_items, 0, sizeof(s_list_items));
    }
}

void demo_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_screen_off) {
        if (ev == BSP_BTN_CLICK) { bsp_display_backlight(100); s_screen_off = false; }
        return;
    }

    if (s_mode == VIEW_LIST) {
        if (ev != BSP_BTN_CLICK) return;
        switch (btn) {
        case BSP_BTN_UP:
            if (s_book_count > 0) {
                s_book_sel = (s_book_sel + s_book_count - 1) % s_book_count;
                if (s_book_sel < s_book_scroll) s_book_scroll = s_book_sel;
                if (s_book_sel >= s_book_scroll + 8) s_book_scroll = s_book_sel - 7;
                refresh_list();
            }
            break;
        case BSP_BTN_DOWN:
            if (s_book_count > 0) {
                s_book_sel = (s_book_sel + 1) % s_book_count;
                if (s_book_sel >= s_book_scroll + 8) s_book_scroll = s_book_sel - 7;
                if (s_book_sel < s_book_scroll) s_book_scroll = s_book_sel;
                refresh_list();
            }
            break;
        case BSP_BTN_OK:
            if (s_book_count > 0) {
                char path[EBOOK_MAX_PATH];
                ebook_reader_get_path(s_books[s_book_sel].name, path, sizeof(path));
                if (ebook_reader_open(&s_reader, path)) {
                    ebook_reader_read_page(&s_reader);
                    show_read();
                    update_page_display();
                }
            }
            break;
        default: break;
        }
    } else {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            if (s_reader.is_open) { ebook_reader_save_progress(&s_reader); ebook_reader_close(&s_reader); }
            s_book_count = ebook_reader_scan_books(s_books, EBOOK_MAX_BOOKS);
            show_list();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        switch (btn) {
        case BSP_BTN_UP:
            if (ebook_reader_prev_page(&s_reader)) { update_page_display(); ebook_reader_save_progress(&s_reader); }
            break;
        case BSP_BTN_DOWN:
            if (ebook_reader_next_page(&s_reader)) { update_page_display(); ebook_reader_save_progress(&s_reader); }
            break;
        case BSP_BTN_OK:
            bsp_display_backlight(0); s_screen_off = true;
            break;
        default: break;
        }
    }
}
