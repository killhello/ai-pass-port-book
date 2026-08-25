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
        "开源软件声明\n\n"
        "本产品中的开源软件许可由各自的\n"
        "权利持有人授予。本声明代表制造商\n"
        "及其可能向您提供产品的当地子公司\n"
        "提供。\n\n"
        "--- 免责声明 ---\n\n"
        "本产品中的开源软件按\"现状\"分发,\n"
        "不附带任何明示或暗示的保证。\n\n"
        "--- 软件列表 ---\n\n"
        "1. ai-passport\n"
        "   版权所有 (c) 2026 BingYan\n"
        "   许可证: MIT\n\n"
        "2. ESP-IDF v5.5.3\n"
        "   版权所有 (c) Espressif Systems\n"
        "   许可证: Apache-2.0\n\n"
        "3. LVGL v9.5.0\n"
        "   版权所有 (c) 2022 LVGL LLC\n"
        "   许可证: MIT\n\n"
        "4. esp_lvgl_port\n"
        "   版权所有 (c) Espressif Systems\n"
        "   许可证: Apache-2.0\n\n"
        "5. esp_codec_dev\n"
        "   版权所有 (c) Espressif Systems\n"
        "   许可证: Apache-2.0\n\n"
        "6. esp_button\n"
        "   版权所有 (c) Espressif Systems\n"
        "   许可证: Apache-2.0\n\n"
        "--- 源码获取 ---\n\n"
        "如果您通过邮件发送书面请求,\n"
        "我们将提供完整的开源源代码。\n"
        "邮箱: by@bingyan.xyz\n\n"
        "本要约自分发产品或固件之日起\n"
        "三年内有效。"
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
