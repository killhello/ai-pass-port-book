// main/demo_wifi.c —— WiFi 设置页：蓝牙(BLE)配网(配网页面为 Web Bluetooth HTML)
// 流程: OK 进入 → 停 WiFi 释放内存 → BLE 广播 AI-Passport → 手机打开
//       ble_provisioning.html 连接并提交 WiFi 凭证 → 自动连接
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "wifi_sta.h"
#include "ble_prov.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
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
    PH_WAIT = 0,       // BLE 广播中, 等手机提交配置
    PH_CONNECTING,     // 收到凭证, 正在连 WiFi
    PH_OK,             // 连接成功且联网验证通过(百度可达)
    PH_NO_NET,         // 连上 WiFi 但无法上网
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
static volatile bool s_task_alive = false;
static lv_timer_t *s_poll;

static void show_info_page(void);

// 联网验证: HTTP GET 百度首页, 拿到 HTML 响应才算真正配网成功
static bool net_check_baidu(void) {
    esp_http_client_config_t cfg = {
        .url = "http://www.baidu.com/",
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_err_t err = esp_http_client_perform(cli);   // 收 HTML 到内部缓冲后丢弃
    int status = esp_http_client_get_status_code(cli);
    int64_t len = esp_http_client_fetch_headers(cli);
    esp_http_client_cleanup(cli);
    ESP_LOGI(TAG, "联网验证: %s status=%d len=%lld",
             esp_err_to_name(err), status, (long long)len);
    // 200 且有响应体 => 拿到了百度 HTML, 真正能上网
    return (err == ESP_OK && status == 200);
}

// ---- 后台任务: 停 WiFi → BLE 收凭证 → 重连 WiFi ----
static void prov_task(void *arg) {
    (void)arg;
    // 从看门狗移除: 初始化/连接可能长时间阻塞, 只允许卡死不允许重启
    esp_task_wdt_delete(NULL);

    wifi_sta_set_suspended(true);       // 挂起开机自动重连, 避免抢 WiFi
    wifi_sta_stop();                    // 释放 WiFi 驱动内存给 BLE 协议栈
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = ble_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败: %s", esp_err_to_name(err));
        wifi_sta_set_suspended(false);
        s_phase = PH_FAIL;
        s_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    char ssid[33] = {0}, pass[65] = {0};
    while (!s_cancel) {
        if (ble_prov_get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    wifi_sta_set_suspended(false);      // 配网结束, 恢复自动重连
    ble_prov_stop();                    // 先释放 BLE 内存再连 WiFi

    if (s_cancel) {
        ESP_LOGI(TAG, "用户取消配网");
        s_phase = PH_FAIL;
        s_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "收到凭证, 连接: %s", ssid);
    s_phase = PH_CONNECTING;
    vTaskDelay(pdMS_TO_TICKS(100));     // 给 UI 一拍刷新"正在连接"

    esp_err_t rc = wifi_sta_connect(ssid, pass[0] ? pass : NULL);
    if (rc == ESP_OK) {
        // WiFi 连上不代表能上网, 请求百度验证
        s_phase = PH_CONNECTING;
        if (net_check_baidu()) {
            s_phase = PH_OK;            // 配网成功: 百度 HTML 已返回
        } else {
            s_phase = PH_NO_NET;        // 连上 WiFi 但无互联网
        }
    } else {
        s_phase = PH_FAIL;
    }
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
        lv_label_set_text(s_prov_status, "配网成功!\n联网验证通过(百度可达)");
        break;
    case PH_NO_NET:
        s_poll = NULL;
        lv_timer_del(t);
        lv_label_set_text(s_prov_hint, "长按 OK 返回");
        lv_label_set_text(s_prov_status, "已连上 WiFi\n但无法上网, 请检查路由器");
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
        lv_label_set_text(s_status, "未连接无线\n按 OK 开始蓝牙配网");
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

    s_prov_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_prov_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_prov_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "蓝牙配网");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_prov_status = lv_label_create(s_prov_scr);
    lv_obj_set_style_text_font(s_prov_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_prov_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_prov_status, 224);
    lv_label_set_long_mode(s_prov_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_prov_status, LV_ALIGN_CENTER, 0, -16);
    lv_label_set_text(s_prov_status,
        "1. 手机打开配网网页\n"
        "   (ble_provisioning.html)\n"
        "2. 点扫描 选 AI-Passport\n"
        "3. 输入WiFi名和密码发送");

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
