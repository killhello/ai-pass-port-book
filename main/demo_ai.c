// main/demo_ai.c —— AI 对话页(三键字符选择器输入)。
// 输入区: 上/下 移动选中字符, OK 追加; 特殊项: 空格/删除/发送
// 发送后进入回复态, OK 返回输入区继续编辑; 长按 OK 由 main 返回菜单。
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "ai_chat.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_ai";

#define TEXT_MAX 120
#define NUM_CHARS 36                     // A-Z + 0-9
#define CELL_TOTAL (NUM_CHARS + 3)       // + 空格/删除/发送
#define IDX_SPACE NUM_CHARS
#define IDX_DEL  (NUM_CHARS + 1)
#define IDX_SEND (NUM_CHARS + 2)
static const char *S_SPECIAL[3] = { "空格", "删除", "发送" };

typedef enum { ST_INPUT = 0, ST_BUSY, ST_REPLY } ai_ui_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_typed;        // 已输入文本
static lv_obj_t *s_cell;         // 当前字符大字
static lv_obj_t *s_hint;         // 底部提示
static lv_obj_t *s_reply;        // 回复正文

static ai_ui_state_t s_state;
static int  s_cell_idx;
static char s_text[TEXT_MAX + 1];
static bool s_busy;

static void refresh_input(void);
static void start_send(void);

static void set_hint(const char *s) {
    lv_label_set_text(s_hint, s);
}

// ---- 回调(ai_chat worker 上下文, 自带 LVGL 锁) ----
static void on_result(ai_state_t state, const char *text) {
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "LVGL 锁超时, 丢弃结果");
        return;
    }
    s_busy = false;
    if (!s_scr) { bsp_lvgl_unlock(); return; }   // 页面已退出

    s_state = ST_REPLY;
    // 切换到回复视图
    lv_obj_add_flag(s_typed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cell, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_reply, LV_OBJ_FLAG_HIDDEN);
    if (state == AI_STATE_OK) {
        lv_label_set_text(s_reply, text);
        set_hint("OK 继续提问");
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "失败:%s\n\n短按 OK 重试", text ? text : "");
        lv_label_set_text(s_reply, buf);
        set_hint("OK 返回输入");
    }
    bsp_lvgl_unlock();
}

// ---- 输入区刷新 ----
static void refresh_input(void) {
    lv_label_set_text(s_typed, s_text[0] ? s_text : "请输入问题:");
    if (s_cell_idx < NUM_CHARS) {
        char c[2] = { "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[s_cell_idx], 0 };
        lv_obj_set_style_text_font(s_cell, &lv_font_montserrat_20, 0);
        lv_label_set_text(s_cell, c);
    } else {
        lv_obj_set_style_text_font(s_cell, &notosanssc_16, 0);
        lv_label_set_text(s_cell, S_SPECIAL[s_cell_idx - NUM_CHARS]);
    }
}

// ---- 发送 ----
static void start_send(void) {
    if (!s_text[0]) { set_hint("先输入内容"); return; }
    if (s_busy) return;
    s_busy = true;
    s_state = ST_BUSY;
    set_hint("请求中...");
    if (!ai_chat_request_async(s_text, on_result)) {
        s_busy = false;
        s_state = ST_INPUT;
        set_hint("请求启动失败");
    }
}

static void cell_action(void) {
    size_t len = strlen(s_text);
    if (s_cell_idx < NUM_CHARS) {
        if (len < TEXT_MAX) {
            s_text[len] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[s_cell_idx];
            s_text[len + 1] = 0;
            refresh_input();
        }
    } else if (s_cell_idx == IDX_SPACE) {
        if (len < TEXT_MAX) {
            s_text[len] = ' '; s_text[len + 1] = 0;
            refresh_input();
        }
    } else if (s_cell_idx == IDX_DEL) {
        if (len) { s_text[len - 1] = 0; refresh_input(); }
    } else {
        start_send();
    }
}

void demo_ai_enter(void) {
    ESP_LOGI(TAG, "enter 开始, 空闲堆 %lu KB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    s_scr = ui_pixel_screen_create("AI 助手");
    ESP_LOGI(TAG, "screen ok");

    // 已输入文本(上)
    lv_obj_t *in_panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 64, UI_PAPER);
    s_typed = lv_label_create(in_panel);
    lv_obj_set_style_text_font(s_typed, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_typed, lv_color_hex(UI_INK), 0);
    lv_obj_set_size(s_typed, 200, 48);
    lv_obj_align(s_typed, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_long_mode(s_typed, LV_LABEL_LONG_WRAP);

    // 当前字符大字(中)
    lv_obj_t *cell_panel = ui_pixel_panel_create(s_scr, 84, 126, 72, 56, UI_PAPER);
    s_cell = lv_label_create(cell_panel);
    lv_obj_center(s_cell);

    // 回复正文(默认隐藏)
    s_reply = ui_pixel_panel_create(s_scr, 10, 48, 220, 248, UI_PAPER);
    lv_obj_t *rl = lv_label_create(s_reply);
    lv_obj_set_style_text_font(rl, &notosanssc_16, 0);
    lv_obj_set_style_text_color(rl, lv_color_hex(UI_INK), 0);
    lv_obj_set_size(rl, 200, 230);
    lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_long_mode(rl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_flag(s_reply, LV_OBJ_FLAG_HIDDEN);
    (void)rl;

    // 底部提示
    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_state = ST_INPUT;
    s_cell_idx = 0;
    refresh_input();
    set_hint("上/下 选字  OK 确定");
    lv_screen_load(s_scr);
}

void demo_ai_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_typed = NULL;
        s_cell = NULL;
        s_reply = NULL;
        s_hint = NULL;
    }
    // 在途请求由 on_result 里 s_scr==NULL 分支自然丢弃
}

void demo_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (s_state == ST_INPUT) {
        if (btn == BSP_BTN_UP) {
            s_cell_idx = (s_cell_idx + CELL_TOTAL - 1) % CELL_TOTAL;
            refresh_input();
        } else if (btn == BSP_BTN_DOWN) {
            s_cell_idx = (s_cell_idx + 1) % CELL_TOTAL;
            refresh_input();
        } else if (btn == BSP_BTN_OK) {
            cell_action();
        }
    } else if (s_state == ST_REPLY) {
        // 返回输入区(保留已输入文本, 可继续编辑/重发)
        s_state = ST_INPUT;
        lv_obj_add_flag(s_reply, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_typed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cell, LV_OBJ_FLAG_HIDDEN);
        refresh_input();
        set_hint("上/下 选字  OK 确定");
    } else if (s_state == ST_BUSY) {
        set_hint("请求中...");   // 忙碌期忽略
    }
}
