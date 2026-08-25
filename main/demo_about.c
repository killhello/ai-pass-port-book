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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *txt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(txt, &notosanssc_16, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_width(txt, 220);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_align(txt, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(txt,
        "FoloToy AI Passport\n"
        "https://github.com/killhello\n\n"
        "本固件使用以下开源项目:\n\n"
        "ESP-IDF (v5.5.3)\n"
        "  Apache-2.0 License\n"
        "  Espressif Systems\n\n"
        "LVGL (v9.5)\n"
        "  MIT License\n"
        "  lvgl.io\n\n"
        "esp_lvgl_port\n"
        "  Apache-2.0 License\n"
        "  Espressif Systems\n\n"
        "esp_codec_dev\n"
        "  Apache-2.0 License\n"
        "  Espressif Systems\n\n"
        "esp_button\n"
        "  Apache-2.0 License\n"
        "  Espressif Systems"
    );

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(hint, "长按 OK 返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);
}

void demo_about_exit(void) {
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
}

void demo_about_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    (void)btn; (void)ev;
}
