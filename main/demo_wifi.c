// main/demo_wifi.c —— WiFi 设置页：一键热点配网
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "wifi_sta.h"
#include "captive_portal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG __attribute__((unused)) = "demo_wifi";

typedef enum {
    STATE_INFO,
    STATE_PROV,
} wifi_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_prov_scr;
static lv_obj_t *s_prov_status;
static lv_obj_t *s_prov_hint;

static wifi_state_t s_state;

static void prov_cb(bool success, void *user) {
    ESP_LOGI(TAG, "配网完成: %s", success ? "成功" : "失败");
}

static void show_info_page(void) {
    s_state = STATE_INFO;

    s_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_title, "WiFi 设置");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 8);

    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_status, 224);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -20);

    if (wifi_sta_is_connected()) {
        lv_label_set_text_fmt(s_status, "已连接: %s", wifi_sta_current_ssid());
    } else {
        lv_label_set_text(s_status, "未连接 WiFi\n按 OK 开始配网");
    }

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_hint, "OK 配网  长按 OK 返回");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);
}

static void show_prov_page(void) {
    s_state = STATE_PROV;

    s_prov_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_prov_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_prov_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "热点配网");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_prov_status = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(s_prov_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_prov_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_prov_status, 220);
    lv_label_set_long_mode(s_prov_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_prov_status, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(s_prov_status,
        "1. 手机连接热点: ESP-WiFi\n"
        "2. 浏览器访问:\n"
        "   http://192.168.4.1\n"
        "3. 选网络 输密码 连接");

    s_prov_hint = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(s_prov_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_prov_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_prov_hint, "长按 OK 取消");
    lv_obj_align(s_prov_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_prov_scr);
}

static void destroy_prov(void) {
    if (s_prov_scr) {
        lv_obj_delete(s_prov_scr);
        s_prov_scr = NULL;
        s_prov_status = NULL;
        s_prov_hint = NULL;
    }
}

void demo_wifi_enter(void) {
    s_state = STATE_INFO;
    show_info_page();
}

void demo_wifi_exit(void) {
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_title = NULL; s_status = NULL; s_hint = NULL; }
    destroy_prov();
    if (captive_portal_is_running()) captive_portal_stop();
}

void demo_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_state == STATE_PROV) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            captive_portal_stop();
            destroy_prov();
            show_info_page();
        }
        return;
    }
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        show_prov_page();
        if (s_prov_status) lv_label_set_text(s_prov_status, "正在启动热点...");
        esp_err_t err = captive_portal_start(prov_cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "配网启动失败: %s", esp_err_to_name(err));
            if (s_prov_status) lv_label_set_text(s_prov_status, "启动失败\n长按 OK 返回");
        }
    }
}
