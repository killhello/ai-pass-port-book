// main/demo_wifi.c —— WiFi 设置页：扫描列表 + 密码键盘 + SmartConfig (ESP-Touch)
// 按键:
//   列表态: UP/DOWN=选项, OK=进入, 长按 OK=返回菜单
//   SmartConfig 态: 显示进度, 长按 OK=取消
//   键盘态: UP/DOWN=移动光标, OK=输入/删格, 长按 OK=确认/返回
#include "demo.h"
#include "font_cn_16.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG __attribute__((unused)) = "demo_wifi";

#define MAX_AP  12

// 键盘布局: 6x6 = 36 个键
static const char *KEYS[] = {
    "A","B","C","D","E","F",
    "G","H","I","J","K","L",
    "M","N","O","P","Q","R",
    "S","T","U","V","W","X",
    "Y","Z","1","2","3","4",
    "5","6","7","8","9","⏎"
};
#define KEY_COUNT 36
#define KEY_CONFIRM_IDX 35

typedef enum {
    STATE_MENU,         // 主菜单：扫描列表 / SmartConfig / BLE Prov
    STATE_LIST,         // 扫描列表
    STATE_KEYBOARD,     // 密码输入
    STATE_CONNECTING,   // 连接中
    STATE_SMARTCONFIG,  // SmartConfig 进行中
    STATE_BLE_PROV,     // BLE Provisioning 进行中
} wifi_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static lv_obj_t *s_items[MAX_AP];
static lv_obj_t *s_hint;

// 键盘相关
static lv_obj_t *s_kbd_scr;
static lv_obj_t *s_kbd_keys[KEY_COUNT];
static lv_obj_t *s_kbd_pwd_label;
static lv_obj_t *s_kbd_hint;
static int s_kbd_sel;
static char s_pwd[65];
static int s_pwd_len;

// SmartConfig 进度
static lv_obj_t *s_sc_scr;
static lv_obj_t *s_sc_status;
static lv_obj_t *s_sc_hint;

// BLE Provisioning 进度
static lv_obj_t *s_bp_scr;
static lv_obj_t *s_bp_status;
static lv_obj_t *s_bp_hint;

static wifi_ap_info_t s_aps[MAX_AP];
static int s_ap_count;
static int s_sel;
static int s_menu_sel;  // 0=扫描列表, 1=SmartConfig
static wifi_state_t s_state;

static void refresh_list(void) {
    for (int i = 0; i < MAX_AP; i++) {
        if (i < s_ap_count) {
            char buf[48];
            const char *lock = (s_aps[i].authmode != WIFI_AUTH_OPEN) ? "🔒 " : "";
            snprintf(buf, sizeof(buf), "%s%s  %ddBm", lock, s_aps[i].ssid, s_aps[i].rssi);
            lv_label_set_text(s_items[i], buf);
            lv_obj_clear_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_list_sel(void) {
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

static void update_menu_sel(void) {
    // 0=扫描列表, 1=SmartConfig
    lv_obj_set_style_text_color(s_items[0],
        s_menu_sel == 0 ? lv_color_hex(UI_SKY) : lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_color(s_items[1],
        s_menu_sel == 1 ? lv_color_hex(UI_SKY) : lv_color_hex(UI_INK), 0);
}

static void do_scan(void) {
    s_state = STATE_LIST;
    lv_label_set_text(s_status, "正在扫描...");
    lv_label_set_text(s_hint, "");
    lv_refr_now(NULL);

    s_ap_count = wifi_sta_scan(s_aps, MAX_AP);

    if (s_ap_count == 0) {
        lv_label_set_text(s_status, "未发现 WiFi 网络");
        lv_label_set_text(s_hint, "短按 OK 重试  长按 OK 返回");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "发现 %d 个网络", s_ap_count);
        lv_label_set_text(s_status, buf);
        lv_label_set_text(s_hint, "上下选网络  OK 连接");
    }

    s_sel = 0;
    refresh_list();
    update_list_sel();
}

static void on_connected(void) {
    s_state = STATE_LIST;
    s_pwd_len = 0;
    s_pwd[0] = '\0';
    lv_label_set_text(s_status, "连接成功!");
    lv_label_set_text(s_hint, "短按 OK 刷新  长按 OK 返回");
}

static void on_connect_fail(void) {
    s_state = STATE_LIST;
    lv_label_set_text(s_status, "连接失败");
    lv_label_set_text(s_hint, "短按 OK 重试  长按 OK 返回");
}

// ===== SmartConfig UI =====
static void sc_create_ui(void) {
    s_sc_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_sc_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_sc_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_sc_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "SmartConfig");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_sc_status = lv_label_create(s_sc_scr);
    lv_obj_set_style_text_font(s_sc_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_sc_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_sc_status, 220);
    lv_label_set_long_mode(s_sc_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_sc_status, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(s_sc_status, "等待手机配网...\n请打开 ESP-Touch App");

    s_sc_hint = lv_label_create(s_sc_scr);
    lv_obj_set_style_text_font(s_sc_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_sc_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_sc_hint, "长按 OK 取消");
    lv_obj_align(s_sc_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_sc_scr);
}

static void sc_update_status(const char *msg) {
    if (s_sc_status) lv_label_set_text(s_sc_status, msg);
}

static void sc_destroy_ui(void) {
    if (s_sc_scr) {
        lv_obj_delete(s_sc_scr);
        s_sc_scr = NULL;
        s_sc_status = NULL;
        s_sc_hint = NULL;
    }
}

// SmartConfig 回调
static void sc_callback(smartconfig_status_t status, void *user) {
    (void)user;
    switch (status) {
        case SC_STATUS_FIND_CHANNEL:
            sc_update_status("正在扫描信道...");
            break;
        case SC_STATUS_GETTING_SSID_PSWD:
            sc_update_status("正在获取 SSID/密码...");
            break;
        case SC_STATUS_LINK:
            sc_update_status("正在连接 WiFi...");
            break;
        case SC_STATUS_LINK_OVER:
            sc_update_status("配网成功!\n已自动连接");
            break;
        case SC_STATUS_FAIL:
            sc_update_status("配网失败/超时\n长按 OK 返回");
            break;
        default:
            break;
    }
}

// ===== BLE Provisioning 回调 =====
static void bp_callback(ble_prov_status_t status, void *user) {
    (void)user;
    switch (status) {
        case BLE_PROV_STATUS_STARTING:
            bp_update_status("正在启动 BLE Provisioning...");
            break;
        case BLE_PROV_STATUS_WAITING_CLIENT:
            bp_update_status("等待手机连接...\n请打开 ESP Provision App");
            break;
        case BLE_PROV_STATUS_RECEIVING_CREDS:
            bp_update_status("正在接收 WiFi 凭证...");
            break;
        case BLE_PROV_STATUS_CONNECTING_WIFI:
            bp_update_status("正在连接 WiFi...");
            break;
        case BLE_PROV_STATUS_SUCCESS:
            bp_update_status("配网成功!\n已自动连接");
            break;
        case BLE_PROV_STATUS_FAIL:
            bp_update_status("配网失败/超时\n长按 OK 返回");
            break;
        default:
            break;
    }
}

// ===== BLE Provisioning UI =====
static void bp_create_ui(void) {
    s_bp_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_bp_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_bp_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_bp_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "BLE Provisioning");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_bp_status = lv_label_create(s_bp_scr);
    lv_obj_set_style_text_font(s_bp_status, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_bp_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_bp_status, 220);
    lv_label_set_long_mode(s_bp_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_bp_status, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(s_bp_status, "正在启动 BLE Provisioning...\n请打开 ESP Provision App");

    s_bp_hint = lv_label_create(s_bp_scr);
    lv_obj_set_style_text_font(s_bp_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_bp_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_bp_hint, "长按 OK 取消");
    lv_obj_align(s_bp_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_bp_scr);
}

static void bp_update_status(const char *msg) {
    if (s_bp_status) lv_label_set_text(s_bp_status, msg);
}

static void bp_destroy_ui(void) {
    if (s_bp_scr) {
        lv_obj_delete(s_bp_scr);
        s_bp_scr = NULL;
        s_bp_status = NULL;
        s_bp_hint = NULL;
    }
}
static void kbd_create(void) {
    s_kbd_scr = ui_pixel_screen_create("WiFi");
    lv_obj_set_style_bg_color(s_kbd_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_kbd_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_kbd_scr);
    lv_obj_set_style_text_font(title, &notosanssc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "输入密码");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_kbd_pwd_label = lv_label_create(s_kbd_scr);
    lv_obj_set_style_text_font(s_kbd_pwd_label, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_kbd_pwd_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_width(s_kbd_pwd_label, 220);
    lv_label_set_long_mode(s_kbd_pwd_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_kbd_pwd_label, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s_kbd_pwd_label, "(空)");

    lv_obj_t *kbd_cont = lv_obj_create(s_kbd_scr);
    lv_obj_set_size(kbd_cont, 230, 180);
    lv_obj_align(kbd_cont, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(kbd_cont, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(kbd_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kbd_cont, 0, 0);
    lv_obj_set_style_radius(kbd_cont, 0, 0);
    lv_obj_set_style_pad_all(kbd_cont, 2, 0);
    lv_obj_set_flex_flow(kbd_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(kbd_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(kbd_cont, 2, 0);
    lv_obj_set_style_pad_column(kbd_cont, 2, 0);

    for (int i = 0; i < KEY_COUNT; i++) {
        s_kbd_keys[i] = lv_btn_create(kbd_cont);
        lv_obj_set_size(s_kbd_keys[i], 36, 26);
        lv_obj_set_style_bg_color(s_kbd_keys[i], lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(s_kbd_keys[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_kbd_keys[i], 3, 0);
        lv_obj_set_style_border_width(s_kbd_keys[i], 0, 0);

        lv_obj_t *lbl = lv_label_create(s_kbd_keys[i]);
        lv_obj_set_style_text_font(lbl, &notosanssc_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
        lv_label_set_text(lbl, KEYS[i]);
        lv_obj_center(lbl);
    }

    s_kbd_hint = lv_label_create(s_kbd_scr);
    lv_obj_set_style_text_font(s_kbd_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_kbd_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_kbd_hint, "上下选键  OK 输入  长按 OK 确认");
    lv_obj_align(s_kbd_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_screen_load(s_kbd_scr);
}

static void kbd_update_sel(void) {
    for (int i = 0; i < KEY_COUNT; i++) {
        bool selected = (i == s_kbd_sel);
        if (selected) {
            lv_obj_set_style_bg_color(s_kbd_keys[i], lv_color_hex(UI_SKY), 0);
            lv_obj_t *lbl = lv_obj_get_child(s_kbd_keys[i], 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(s_kbd_keys[i], lv_color_hex(0x222222), 0);
            lv_obj_t *lbl = lv_obj_get_child(s_kbd_keys[i], 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK), 0);
        }
    }
}

static void kbd_update_pwd(void) {
    if (s_pwd_len == 0) {
        lv_label_set_text(s_kbd_pwd_label, "(空)");
    } else {
        char stars[65];
        for (int i = 0; i < s_pwd_len; i++) stars[i] = '*';
        stars[s_pwd_len] = '\0';
        lv_label_set_text(s_kbd_pwd_label, stars);
    }
}

static void kbd_destroy(void) {
    if (s_kbd_scr) {
        lv_obj_delete(s_kbd_scr);
        s_kbd_scr = NULL;
        s_kbd_pwd_label = NULL;
        s_kbd_hint = NULL;
        for (int i = 0; i < KEY_COUNT; i++) s_kbd_keys[i] = NULL;
    }
}

// ===== 状态机 =====
static void enter_keyboard(void) {
    s_state = STATE_KEYBOARD;
    s_pwd_len = 0;
    s_pwd[0] = '\0';
    s_kbd_sel = 0;
    kbd_create();
    kbd_update_sel();
    kbd_update_pwd();
}

static void enter_smartconfig(void) {
    s_state = STATE_SMARTCONFIG;
    sc_create_ui();
    esp_err_t err = wifi_sta_smartconfig_start(sc_callback, NULL, 60000);
    if (err != ESP_OK) {
        lv_label_set_text(s_sc_status, "启动失败");
        lv_label_set_text(s_sc_hint, "长按 OK 返回");
    }
}

static void enter_ble_prov(void) {
    s_state = STATE_BLE_PROV;
    bp_create_ui();
    esp_err_t err = wifi_sta_ble_prov_start(bp_callback, NULL, 60000);
    if (err != ESP_OK) {
        bp_update_status("启动失败\n长按 OK 返回");
    }
}

static void enter_connecting(const char *ssid, const char *pwd) {
    s_state = STATE_CONNECTING;
    kbd_destroy();

    lv_label_set_text(s_status, "正在连接...");
    lv_label_set_text(s_hint, "");
    lv_refr_now(NULL);

    esp_err_t err = wifi_sta_connect(ssid, pwd);
    if (err == ESP_OK) {
        on_connected();
    } else {
        on_connect_fail();
    }
}

static void list_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        if (s_ap_count > 0) {
            s_sel = (s_sel + s_ap_count - 1) % s_ap_count;
            update_list_sel();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_ap_count > 0) {
            s_sel = (s_sel + 1) % s_ap_count;
            update_list_sel();
        }
    } else if (btn == BSP_BTN_OK) {
        if (s_ap_count == 0) {
            do_scan();
        } else {
            if (s_aps[s_sel].authmode == WIFI_AUTH_OPEN) {
                s_state = STATE_CONNECTING;
                lv_label_set_text(s_status, "正在连接...");
                lv_label_set_text(s_hint, "");
                lv_refr_now(NULL);

                esp_err_t err = wifi_sta_connect(s_aps[s_sel].ssid, NULL);
                if (err == ESP_OK) on_connected(); else on_connect_fail();
            } else {
                enter_keyboard();
            }
        }
    }
}

static void menu_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s_menu_sel = (s_menu_sel + 2) % 3;  // 0,1,2 循环
        update_menu_sel();
    } else if (btn == BSP_BTN_DOWN) {
        s_menu_sel = (s_menu_sel + 1) % 3;
        update_menu_sel();
    } else if (btn == BSP_BTN_OK) {
        if (s_menu_sel == 0) {
            // 扫描列表
            s_state = STATE_LIST;
            do_scan();
        } else if (s_menu_sel == 1) {
            // SmartConfig
            enter_smartconfig();
        } else {
            // BLE Provisioning
            enter_ble_prov();
        }
    }
}

static void sc_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        wifi_sta_smartconfig_stop();
        sc_destroy_ui();
        s_state = STATE_MENU;
        // 回到主菜单
        demo_wifi_enter();
    }
}

static void bp_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        wifi_sta_ble_prov_stop();
        bp_destroy_ui();
        s_state = STATE_MENU;
        // 回到主菜单
        demo_wifi_enter();
    }
}

static void kbd_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        s_kbd_sel = (s_kbd_sel + KEY_COUNT - 1) % KEY_COUNT;
        kbd_update_sel();
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        s_kbd_sel = (s_kbd_sel + 1) % KEY_COUNT;
        kbd_update_sel();
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        if (s_kbd_sel == KEY_CONFIRM_IDX) {
            enter_connecting(s_aps[s_sel].ssid, s_pwd);
        } else if (s_pwd_len < 64) {
            const char *ch = KEYS[s_kbd_sel];
            if (ch[0] != '\0' && ch[1] == '\0') {
                s_pwd[s_pwd_len++] = ch[0];
                s_pwd[s_pwd_len] = '\0';
                kbd_update_pwd();
            }
        }
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_pwd_len > 0) {
            s_pwd[--s_pwd_len] = '\0';
            kbd_update_pwd();
        } else {
            kbd_destroy();
            s_state = STATE_LIST;
            do_scan();
        }
    }
}

void demo_wifi_enter(void) {
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
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s_status, "请选择配网方式");

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

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_hint, "上下选择  OK 确认");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);

    s_state = STATE_MENU;
    s_menu_sel = 0;
    // 初始化菜单项
    lv_label_set_text(s_items[0], "📡 扫描列表连接");
    lv_label_set_text(s_items[1], "📱 SmartConfig (ESP-Touch)");
    lv_label_set_text(s_items[2], "📶 BLE Provisioning");
    lv_obj_clear_flag(s_items[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_items[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_items[2], LV_OBJ_FLAG_HIDDEN);
    for (int i = 3; i < MAX_AP; i++) {
        lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_ap_count = 3;  // 菜单模式下显示 3 项
    update_menu_sel();
    lv_label_set_text(s_status, "请选择配网方式");
    lv_label_set_text(s_hint, "上下选择  OK 确认");
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
    kbd_destroy();
    sc_destroy_ui();
    bp_destroy_ui();
    if (wifi_sta_smartconfig_is_running()) {
        wifi_sta_smartconfig_stop();
    }
    if (wifi_sta_ble_prov_is_running()) {
        wifi_sta_ble_prov_stop();
    }
    s_state = STATE_MENU;
}

void demo_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_state == STATE_CONNECTING) return;

    if (s_state == STATE_KEYBOARD) {
        kbd_handle_key(btn, ev);
    } else if (s_state == STATE_SMARTCONFIG) {
        sc_handle_key(btn, ev);
    } else if (s_state == STATE_BLE_PROV) {
        bp_handle_key(btn, ev);
    } else if (s_state == STATE_MENU) {
        menu_handle_key(btn, ev);
    } else {
        list_handle_key(btn, ev);
    }
}