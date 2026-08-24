// main/demo_ai.c —— AI 对话页(与 demo_ebook 相同的页面机制)。
// 进入页面自动发送"你好",显示模型回复;短按 OK 重新发送;长按 OK 由 main 返回菜单。
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "ai_chat.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_ai";

static lv_obj_t *s_scr;
static lv_obj_t *s_text;      // 状态/回复正文
static lv_obj_t *s_hint;      // 底部操作提示
static bool s_busy;           // 在途请求

static void set_text(const char *s) {
    lv_label_set_text(s_text, s);
}

static void send_hello(void);

static void on_result(ai_state_t state, const char *text) {
    // worker 任务上下文:必须自己加锁
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "拿 LVGL 锁超时,丢弃结果");
        s_busy = false;
        return;
    }
    s_busy = false;
    if (!s_scr) { bsp_lvgl_unlock(); return; }   // 页面已退出

    if (state == AI_STATE_OK) {
        set_text(text);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "失败:%s\n\n短按 OK 重试,长按 OK 返回", text);
        set_text(buf);
    }
    bsp_lvgl_unlock();
}

static void send_hello(void) {
    if (s_busy) return;
    s_busy = true;
    set_text("正在连接 AI\n请稍候...");
    if (!ai_chat_request_async("你好", on_result)) {
        s_busy = false;
        set_text("请求启动失败\n\n短按 OK 重试,长按 OK 返回");
    }
}

void demo_ai_enter(void) {
    s_scr = ui_pixel_screen_create("AI");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_text = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_text, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_text, 216);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 44);
    set_text("正在连接 AI\n请稍候...");

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_hint, "短按 OK 重发  长按 OK 返回");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);

    send_hello();
}

void demo_ai_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_text = NULL;
        s_hint = NULL;
    }
    // 在途请求由 on_result 里 s_scr==NULL 分支自然丢弃
}

void demo_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        send_hello();
    }
}
