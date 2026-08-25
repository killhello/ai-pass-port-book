// main/demo_about.c —— 关于页：开源声明
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG __attribute__((unused)) = "demo_about";

static lv_obj_t *s_scr;

void demo_about_enter(void) {
    s_scr = ui_pixel_screen_create("关于");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "开源声明");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *scroll = lv_obj_create(s_scr);
    lv_obj_set_size(scroll, 230, 260);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(scroll, LV_SCROLL_SNAP_START);

    lv_obj_t *txt = lv_label_create(scroll);
    lv_obj_set_style_text_font(txt, &notosanssc_16, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_width(txt, 220);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(txt, 5, 0);
    lv_label_set_text(txt,
        "OPEN SOURCE SOFTWARE NOTICE\n\n"
        "The open source software licenses are\n"
        "granted by the respective right holders.\n"
        "This notice is provided on behalf of\n"
        "the manufacturer.\n\n"
        "--- Warranty Disclaimer ---\n\n"
        "THE OPEN SOURCE SOFTWARE IN THIS\n"
        "PRODUCT IS DISTRIBUTED IN THE HOPE\n"
        "THAT IT WILL BE USEFUL, BUT WITHOUT\n"
        "ANY WARRANTY.\n\n"
        "--- Software List ---\n\n"
        "1. ai-passport\n"
        "   Copyright (c) 2026 BingYan\n"
        "   License: MIT\n\n"
        "2. ESP-IDF v5.5.3\n"
        "   Copyright (c) Espressif Systems\n"
        "   License: Apache-2.0\n\n"
        "3. LVGL v9.5.0\n"
        "   Copyright (c) 2022 LVGL LLC\n"
        "   License: MIT\n\n"
        "4. esp_lvgl_port\n"
        "   Copyright (c) Espressif Systems\n"
        "   License: Apache-2.0\n\n"
        "5. esp_codec_dev\n"
        "   Copyright (c) Espressif Systems\n"
        "   License: Apache-2.0\n\n"
        "6. esp_button\n"
        "   Copyright (c) Espressif Systems\n"
        "   License: Apache-2.0\n\n"
        "--- Written Offer ---\n\n"
        "We will provide the complete source\n"
        "code licensed under open source\n"
        "license if you send a written request\n"
        "by email to: by@bingyan.xyz\n\n"
        "This offer is valid for three years\n"
        "from the moment we distributed the\n"
        "product or firmware."
    );

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint, "上下滚动 长按OK返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_screen_load(s_scr);
}

void demo_about_exit(void) {
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
}

void demo_about_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    (void)btn; (void)ev;
}
