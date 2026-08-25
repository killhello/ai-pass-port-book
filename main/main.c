// main/main.c —— FoloToy-Card BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "demo.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "font_cn_16.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "boot_animation.h"

static const char *TAG = "main";

// 简单 WiFi 扫描测试任务(串口直接看结果，无 UI 干扰)
static void wifi_test_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));  // 等待系统稳定
    
    ESP_LOGI(TAG, "=== WiFi 硬件测试开始 ===");
    
    // 1. 初始化网络栈
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    // 2. 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // 设置国家码
    wifi_country_t country = { .cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO };
    esp_wifi_set_country(&country);
    
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 3. 禁用省电模式
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    // 4. 扫描测试
    ESP_LOGI(TAG, "开始扫描...");
    wifi_scan_config_t scan_cfg = { 0 };
    scan_cfg.show_hidden = false;
    
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
    } else {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "扫描完成，发现 %d 个 AP", ap_count);
        
        if (ap_count > 0) {
            uint16_t fetch = ap_count < 20 ? ap_count : 20;
            wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
            if (records) {
                esp_wifi_scan_get_ap_records(&fetch, records);
                for (int i = 0; i < fetch; i++) {
                    ESP_LOGI(TAG, "  [%d] SSID: %-32s  RSSI: %4d  Auth: %d  Channel: %d",
                             i, records[i].ssid, records[i].rssi, records[i].authmode, records[i].primary);
                }
                free(records);
            }
        }
    }
    
    ESP_LOGI(TAG, "=== WiFi 硬件测试结束 ===");
    vTaskDelete(NULL);
}

static const demo_entry_t DEMOS[] = {
    { "显示", demo_display_enter, demo_display_exit, demo_display_key },
    { "按键", demo_button_enter,  demo_button_exit,  demo_button_key  },
    { "音频", demo_audio_enter,   demo_audio_exit,   demo_audio_key   },
    { "电池", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "WiFi", demo_wifi_enter,    demo_wifi_exit,    demo_wifi_key    },
    { "电子书", demo_ebook_enter, demo_ebook_exit,   demo_ebook_key   },
    { "AI",     demo_ai_enter,    demo_ai_exit,      demo_ai_key      },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

static bool s_ok[DEMO_COUNT];
static bool s_spiffs_ok;

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static int  s_sel;
static int  s_active = -1;

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : "  [FAIL]");
        ui_pixel_set_selected(s_cards[i], (int)i == s_sel, s_ok[i]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("FoloToy");

    // 还原原版 2×3 网格(前 6 项),第 7 项(WiFi)单独居中放第 4 行
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x, y, w = 102, h = 72;
        if (i < 6) {
            // 原版 2 列 × 3 行
            x = 11 + (int)(i % 2) * 112;
            y = 46 + (int)(i / 2) * 78;
        } else {
            // 第 7 项:居中单独一行
            x = 11;
            y = 46 + 3 * 78;
            w = 224;
        }
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, w, h, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &notosanssc_16, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 280);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT;              menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            ui_pixel_mascot_jump(s_mascot);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            s_mascot = NULL;
            DEMOS[s_active].enter();
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            ui_pixel_mascot_jump(s_mascot);
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "=== 启动 ===");

    bsp_i2c_init();
    bsp_i2c_scan();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重格式化: %s", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败: %s(进度保存不可用)", esp_err_to_name(nvs_err));
    }

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight(100);

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_conf);
    s_spiffs_ok = (err == ESP_OK);
    if (!s_spiffs_ok) {
        ESP_LOGW(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SPIFFS 挂载成功");
    }

    boot_animation_play();

    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }

    // 启动 WiFi 硬件测试任务(串口直接看结果)
    xTaskCreate(wifi_test_task, "wifi_test", 4096, NULL, 5, NULL);

    s_ok[0] = true;                                   // 显示
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);  // 按键
    s_ok[2] = (bsp_audio_init() == ESP_OK);           // 音频
    s_ok[3] = (bsp_battery_init() == ESP_OK);         // 电池
    s_ok[4] = true;                                   // WiFi(自身处理失败)
    s_ok[5] = s_spiffs_ok;                            // 电子书
    s_ok[6] = true;                                   // AI

    if (bsp_lvgl_lock(1000)) { enter_menu(); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "=== 启动完成 ===");
}