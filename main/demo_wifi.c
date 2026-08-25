// main/demo_wifi.c —— WiFi 设置页:扫描开放网络 + 选择连接。
// 按键:UP/DOWN=选择, OK=连接/重新扫描, 长按 OK=返回菜单
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "demo_wifi";
(void)TAG;  // 仅为消除 unused 变量警告

#define MAX_AP  12

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_items[MAX_AP];
static lv_obj_t *s_hint;

static wifi_ap_info_t s_aps[MAX_AP];
static int s_ap_count;
static int s_sel;
static bool s_connecting;

static void refresh_list(void) {
    for (int i = 0; i < MAX_AP; i++) {
        if (i < s_ap_count) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s  %ddBm", s_aps[i].ssid, s_aps[i].rssi);
            lv_label_set_text(s_items[i], buf);
            lv_obj_clear_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_sel(void) {
    for (int i = 0; i < s_ap_count; i++) {
        bool selected = (i == s_sel);
        lv_obj_set_style_text_color(s_items[i],
            selected ? lv_color_hex(UI_SKY) : lv_color_hex(UI_INK), 0);
        if (selected) {
            lv_obj_set_style_text_decor(s_items[i], LV_TEXT_DECOR_UNDERLINE, 0);
        } else {
            lv_obj_set_style_text_decor(s_items[i], LV_TEXT_DECOR_NONE, 0);
        }
    }
}

static void do_scan(void) {
    s_connecting = false;
    lv_label_set_text(s_status, "正在扫描...");
    lv_label_set_text(s_hint, "");
    lv_refr_now(NULL);

    if (!wifi_sta_start_scan()) {
        lv_label_set_text(s_status, "扫描启动失败");
        lv_label_set_text(s_hint, "短按 OK 重试  长按 OK 返回");
        return;
    }

    s_ap_count = wifi_sta_get_scan_results(s_aps, MAX_AP, 8000);

    if (s_ap_count == 0) {
        lv_label_set_text(s_status, "未发现开放网络");
        lv_label_set_text(s_hint, "短按 OK 重试  长按 OK 返回");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "发现 %d 个开放网络", s_ap_count);
        lv_label_set_text(s_status, buf);
        lv_label_set_text(s_hint, "选择网络  短按 OK 连接");
    }

    s_sel = 0;
    refresh_list();
    update_sel();
}

static void on_connected(void) {
    s_connecting = false;
    lv_label_set_text(s_status, "连接成功!");
    lv_label_set_text(s_hint, "短按 OK 刷新  长按 OK 返回");
}

static void on_connect_fail(void) {
    s_connecting = false;
    lv_label_set_text(s_status, "连接失败");
    lv_label_set_text(s_hint, "短按 OK 重试  长按 OK 返回");
}

void demo_wifi_enter(void) {
    s_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // 标题
    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_title, "WiFi 设置");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 8);

    // 状态
    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_status, 224);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s_status, "准备扫描...");

    // 网络列表
    s_list = lv_obj_create(s_scr);
    lv_obj_set_size(s_list, 224, 180);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (int i = 0; i < MAX_AP; i++) {
        s_items[i] = lv_label_create(s_list);
        lv_obj_set_style_text_font(s_items[i], &notosanssc_16, 0);
        lv_obj_set_style_text_color(s_items[i], lv_color_hex(UI_INK), 0);
        lv_obj_set_style_bg_color(s_items[i], lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(s_items[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_items[i], 6, 0);
        lv_obj_set_style_pad_ver(s_items[i], 3, 0);
        lv_obj_set_width(s_items[i], 224);
        lv_label_set_text(s_items[i], "");
        lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 提示
    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);

    do_scan();
}

void demo_wifi_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_title = NULL;
        s_status = NULL;
        s_list = NULL;
        s_hint = NULL;
    }
    s_connecting = false;
}

void demo_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (s_connecting) return;   // 连接中忽略按键

    if (btn == BSP_BTN_UP) {
        if (s_ap_count > 0) {
            s_sel = (s_sel + s_ap_count - 1) % s_ap_count;
            update_sel();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_ap_count > 0) {
            s_sel = (s_sel + 1) % s_ap_count;
            update_sel();
        }
    } else if (btn == BSP_BTN_OK) {
        if (s_ap_count == 0) {
            do_scan();
        } else {
            // 连接选中的网络
            s_connecting = true;
            char buf[64];
            snprintf(buf, sizeof(buf), "正在连接 %s ...", s_aps[s_sel].ssid);
            lv_label_set_text(s_status, buf);
            lv_label_set_text(s_hint, "");
            lv_refr_now(NULL);

            esp_err_t err = wifi_sta_connect_to(s_aps[s_sel].ssid);
            if (err == ESP_OK) {
                on_connected();
            } else {
                on_connect_fail();
            }
        }
    }
}
