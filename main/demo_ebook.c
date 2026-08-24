// main/demo_ebook.c —— 电子书阅读器页面。
// 按键:UP=上一页, DOWN=下一页, OK=息屏/亮屏
// 进度:每次翻页自动保存到 NVS,下次打开自动恢复
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

static lv_obj_t *s_scr;
static lv_obj_t *s_text;        // 正文显示
static lv_obj_t *s_page_info;   // 页码信息
static lv_obj_t *s_title;       // 标题

static ebook_reader_t s_reader;
static bool s_screen_off = false;  // 息屏状态

// 默认电子书路径(可根据实际情况修改)
#define DEFAULT_EBOOK_PATH  "/spiffs/book.txt"

static void update_page_display(void) {
    if (!s_reader.is_open) {
        lv_label_set_text(s_text, "未找到电子书文件\n\n请将 .txt 文件放入\nSPIFFS 分区");
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

static void turn_screen_off(void) {
    if (s_screen_off) return;
    bsp_display_backlight(0);
    s_screen_off = true;
    ESP_LOGI(TAG, "屏幕已熄灭");
}

static void turn_screen_on(void) {
    if (!s_screen_off) return;
    bsp_display_backlight(100);
    s_screen_off = false;
    ESP_LOGI(TAG, "屏幕已点亮");
}

void demo_ebook_enter(void) {
    s_screen_off = false;

    // 创建屏幕
    s_scr = ui_pixel_screen_create("E-Book");

    // 正文面板
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    // 正文标签
    s_text = lv_label_create(panel);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_size(s_text, 204, 224);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);

    // 底部页码信息
    s_page_info = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_page_info, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(s_page_info, "-- / --");

    // 中文字体已编译进固件,直接使用
    lv_obj_set_style_text_font(s_text, &notosanssc_16, 0);
    lv_obj_set_style_text_font(s_page_info, &notosanssc_16, 0);

    lv_screen_load(s_scr);

    // 初始化阅读器
    ebook_reader_init(&s_reader);

    // 先尝试加载上次阅读进度
    if (ebook_reader_load_progress(&s_reader)) {
        ESP_LOGI(TAG, "已恢复上次阅读进度");
    } else {
        // 没有保存的进度,尝试打开默认书籍
        ESP_LOGI(TAG, "无保存进度,尝试打开默认书籍");
        ebook_reader_open(&s_reader, DEFAULT_EBOOK_PATH);
        ebook_reader_read_page(&s_reader);
    }

    update_page_display();

    ESP_LOGI(TAG, "电子书阅读器已启动");
}

void demo_ebook_exit(void) {
    // 退出前保存进度
    if (s_reader.is_open) {
        ebook_reader_save_progress(&s_reader);
        ebook_reader_close(&s_reader);
    }

    // 确保亮屏
    if (s_screen_off) {
        turn_screen_on();
    }

    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_text = NULL;
        s_page_info = NULL;
        s_title = NULL;
    }

    ESP_LOGI(TAG, "电子书阅读器已退出");
}

void demo_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 息屏状态下任意按键都点亮屏幕
    if (s_screen_off) {
        if (ev == BSP_BTN_CLICK) {
            turn_screen_on();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) return;  // 只响应单击

    switch (btn) {
        case BSP_BTN_UP:
            if (ebook_reader_prev_page(&s_reader)) {
                update_page_display();
                ebook_reader_save_progress(&s_reader);
            }
            break;

        case BSP_BTN_DOWN:
            if (ebook_reader_next_page(&s_reader)) {
                update_page_display();
                ebook_reader_save_progress(&s_reader);
            }
            break;

        case BSP_BTN_OK:
            // OK 键 = 息屏
            turn_screen_off();
            break;

        default:
            break;
    }
}
