// main/demo_about.c —— 关于页:开源软件声明,复用电子书阅读器打开 /spiffs/about.txt
// 按键:UP=上一页, DOWN=下一页, 长按 OK 由 main 返回菜单
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "ebook_reader.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_about";

#define ABOUT_PATH  "/spiffs/about.txt"

static lv_obj_t *s_scr;
static lv_obj_t *s_text;      // 正文
static lv_obj_t *s_page_info; // 页码

static ebook_reader_t s_reader;

static void show_page(void) {
    if (!s_reader.is_open) {
        lv_label_set_text(s_text, "未找到 about.txt\n\n请确认固件包完整");
        lv_label_set_text(s_page_info, "-- / --");
        return;
    }
    lv_label_set_text(s_text, s_reader.page_buf);
    char info[32];
    snprintf(info, sizeof(info), "%lu / %lu",
             (unsigned long)(s_reader.current_page + 1),
             (unsigned long)s_reader.total_pages);
    lv_label_set_text(s_page_info, info);
}

void demo_about_enter(void) {
    // 页眉标题直接显示文档名(中文, 由 ui_pixel_screen_create 渲染)
    s_scr = ui_pixel_screen_create("开源软件声明");

    // 正文面板(与电子书页一致)
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 248, UI_PAPER);
    s_text = lv_label_create(panel);
    lv_obj_set_style_text_font(s_text, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_size(s_text, 204, 232);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);

    // 底部页码
    s_page_info = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_page_info, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_page_info, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_page_info, "-- / --");

    lv_screen_load(s_scr);

    ebook_reader_init(&s_reader);
    if (ebook_reader_open(&s_reader, ABOUT_PATH)) {
        ebook_reader_read_page(&s_reader);
        ESP_LOGI(TAG, "已打开 %s", ABOUT_PATH);
    } else {
        ESP_LOGE(TAG, "打开 %s 失败", ABOUT_PATH);
    }
    show_page();
}

void demo_about_exit(void) {
    if (s_reader.is_open) ebook_reader_close(&s_reader);   // 不保存进度
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_text = NULL;
        s_page_info = NULL;
    }
}

void demo_about_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    switch (btn) {
    case BSP_BTN_UP:
        if (ebook_reader_prev_page(&s_reader)) show_page();
        break;
    case BSP_BTN_DOWN:
        if (ebook_reader_next_page(&s_reader)) show_page();
        break;
    default:
        break;
    }
}
