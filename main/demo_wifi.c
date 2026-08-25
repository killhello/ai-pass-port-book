// main/demo_wifi.c —— WiFi 设置页：热点配网(AP + HTML 配网页面)
// 流程: OK 进入 → 停 WiFi 释放内存 → 开热点 ESP-WiFi → 手机浏览器 192.168.4.1 配置
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "wifi_sta.h"
#include "captive_portal.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG __attribute__((unused)) = "demo_wifi";

typedef enum {
    STATE_INFO,
    STATE_PROV,
} wifi_state_t;

// 配网后台任务阶段（volatile, LVGL 定时器轮询）
typedef enum {
    PH_WAIT = 0,       // 热点运行中, 等手机提交配置
    PH_CONNECTING,     // 收到配置, 正在连 WiFi
    PH_OK,             // 连接成功
    PH_FAIL,           // 失败/取消结束
} prov_phase_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_prov_scr;
static lv_obj_t *s_prov_status;
static lv_obj_t *s_prov_hint;

static wifi_state_t s_state;
static volatile prov_phase_t s_phase = PH_WAIT;
static volatile bool s_cancel = false;
static volatile bool s_submitted = false;   // 手机已提交配置(captive portal 回调置位)
static volatile bool s_task_alive = false;
static lv_timer_t *s_poll;

static void show_info_page(void);

static void prov_cb(bool success, void *user) {
    (void)success; (void)user;
    s_submitted = true;
}

// ---- 后台任务: 停 WiFi → 开热点收配置 → 重连 WiFi ----
static void prov_task(void *arg) {
    (void)arg;
    // 从看门狗移除: 初始化/连接可能长时间阻塞, 只允许卡死不允许重启
    esp_task_wdt_delete(NULL);

    wifi_sta_stop();                    // 释放 WiFi 驱动内存给 AP+HTTP
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = captive_portal_start(prov_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
        s_phase = PH_FAIL;
        s_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    while (!s_cancel && !s_submitted) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    captive_portal_stop();              // 停热点, 释放内存

    if (s_cancel) {
        ESP_LOGI(TAG, "用户取消配网");
        s_phase = PH_FAIL;
        s_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "收到配置, 连接...");
    s_phase = PH_CONNECTING;
    vTaskDelay(pdMS_TO_TICKS(100));     // 给 UI 一拍刷新"正在连接"

    // 凭证已由 captive portal 存入 NVS, 直接加载并连接
    esp_err_t rc = wifi_sta_connect_default();
    s_phase = (rc == ESP_OK) ? PH_OK : PH_FAIL;
    ESP_LOGI(TAG, "连接结果: %s", esp_err_to_name(rc));
    s_task_alive = false;
    vTaskDelete(NULL);
}

// ---- LVGL 定时器: 轮询后台阶段刷新提示 ----
static void poll_cb(lv_timer_t *t) {
    if (!s_prov_status) return;
    switch (s_phase) {
    case PH_WAIT:
        lv_label_set_text(s_prov_status,
            "热点已开启, 等待手机配置...");
        break;
    case PH_CONNECTING:
        lv_label_set_text(s_prov_status, "收到凭证 正在连接...");
        break;
    case PH_OK:
        s_poll = NULL;
        lv_timer_del(t);
        lv_label_set_text(s_prov_hint, "长按 OK 返回");
        lv_label_set_text(s_prov_status, "连接成功!");
        break;
    case PH_FAIL:
        s_poll = NULL;
        lv_timer_del(t);
        lv_label_set_text(s_prov_hint, "长按 OK 返回");
        lv_label_set_text(s_prov_status, "配网失败或已取消");
        break;
    }
}

static void show_info_page(void) {
    s_state = STATE_INFO;

    s_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_title, "无线设置");
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
        lv_label_set_text(s_status, "未连接无线\n按 OK 开始热点配网");
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
    s_phase = PH_WAIT;
    s_cancel = false;
    s_submitted = false;

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
    lv_obj_set_width(s_prov_status, 224);
    lv_label_set_long_mode(s_prov_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_prov_status, LV_ALIGN_CENTER, 0, -16);
    lv_label_set_text(s_prov_status,
        "1. 手机连接热点 ESP-WiFi\n"
        "2. 浏览器打开:\n"
        "   http://192.168.4.1\n"
        "3. 选网络 输密码 连接");

    s_prov_hint = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(s_prov_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_prov_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_prov_hint, "长按 OK 取消");
    lv_obj_align(s_prov_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_prov_scr);

    s_poll = lv_timer_create(poll_cb, 500, NULL);

    if (s_task_alive) {
        // 上一轮任务还在跑(连接中), 不允许重复进入
        lv_label_set_text(s_prov_status, "上一轮配网还在进行\n请稍后再试");
        return;
    }
    s_task_alive = true;
    BaseType_t ret = xTaskCreate(prov_task, "bleprov", 8192, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "配网任务创建失败");
        lv_label_set_text(s_prov_status, "内存不足\n长按 OK 返回");
        s_phase = PH_FAIL;
        s_task_alive = false;
    }
}

static void destroy_prov(void) {
    if (s_poll) { lv_timer_del(s_poll); s_poll = NULL; }
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
}

void demo_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_state == STATE_PROV) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            if (s_phase == PH_WAIT) {
                // 热点等待阶段可安全取消(任务 300ms 内退出)
                s_cancel = true;
                destroy_prov();
                show_info_page();
            }
            // 连接阶段不可中断, 等任务自行结束
        }
        return;
    }
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK && !wifi_sta_is_connected()) {
        show_prov_page();
    }
}
