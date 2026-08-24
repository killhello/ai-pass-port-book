// main/ai_page.c —— AI 对话页:标题 + 状态/回复文本。
// 文本用 SPIFFS 里的中文字体(NotoSansSC_16.bin),缺失时回退 montserrat。
#include "ai_page.h"
#include "ai_config.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ai_page";

#define CN_FONT_PATH "/spiffs/fonts/NotoSansSC_16.bin"

static lv_obj_t *s_scr;
static lv_obj_t *s_text;      // 状态/回复正文
static lv_obj_t *s_hint;      // 底部操作提示
static const lv_font_t *s_cn;  // 运行时加载的中文字体
static bool s_busy;            // 在途请求

static void set_text(const char *s) {
    lv_label_set_text(s_text, s);
}

static void on_result(ai_state_t state, const char *text) {
    // worker 任务上下文:必须自己加锁
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "拿 LVGL 锁超时,丢弃结果");
        s_busy = false;
        return;
    }
    s_busy = false;
    if (!s_scr) { bsp_lvgl_unlock(); return; }   // 页面已被关掉

    if (state == AI_STATE_OK) {
        set_text(text);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "失败:%s\n\n长按 OK 返回", text);
        set_text(buf);
    }
    bsp_lvgl_unlock();
}

static void load_font(void) {
    if (s_cn) return;
    s_cn = lv_binfont_create(CN_FONT_PATH);
    if (!s_cn) ESP_LOGW(TAG, "中文字体加载失败:%s", CN_FONT_PATH);
}

void ai_page_open(void) {
    if (s_scr || s_busy) return;

    load_font();
    const lv_font_t *font = s_cn ? s_cn : &lv_font_montserrat_14;

    s_scr = ui_pixel_screen_create("AI");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_text = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_text, font, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_text, 216);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 44);
    set_text("正在连接 AI\n请稍候...");

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, font, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_hint, "长按 OK 返回");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);

    s_busy = true;
    if (!ai_chat_request_async("你好", on_result)) {
        s_busy = false;
        set_text("请求启动失败\n\n长按 OK 返回");
    }
}

void ai_page_close(void) {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_text = NULL;
        s_hint = NULL;
    }
    bsp_lvgl_unlock();
}

bool ai_page_is_open(void) {
    return s_scr != NULL;
}
