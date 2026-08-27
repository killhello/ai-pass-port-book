// main/demo_image.c —— 图片浏览页面。
// 上键=上一张, 下键=下一张, OK=返回菜单
#include "demo.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "demo_image";
#define IMAGE_DIR "/spiffs/images"
#define MAX_IMAGES 64
#define NAME_LEN 64

static lv_obj_t *s_scr;
static lv_obj_t *s_img;
static lv_obj_t *s_info;
static lv_obj_t *s_hint;

static char s_images[MAX_IMAGES][NAME_LEN];
static int s_count = 0;
static int s_sel = 0;
static bool s_img_valid = false;

static void scan_images(void) {
    s_count = 0;
    DIR *dir = opendir(IMAGE_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MAX_IMAGES) {
        if (ent->d_type == DT_REG) {
            const char *name = ent->d_name;
            size_t nlen = strlen(name);
            if (nlen > 4 && (strcasecmp(name + nlen - 4, ".bmp") == 0 ||
                             strcasecmp(name + nlen - 4, ".jpg") == 0 ||
                             strcasecmp(name + nlen - 5, ".jpeg") == 0 ||
                             strcasecmp(name + nlen - 4, ".png") == 0)) {
                strlcpy(s_images[s_count], name, NAME_LEN);
                s_count++;
            }
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描到 %d 张图片", s_count);
}

static void show_image(int idx) {
    if (!s_scr || idx < 0 || idx >= s_count) return;

    s_img_valid = false;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, s_images[idx]);

    // 尝试用 LVGL 图片解码器加载
    lv_img_set_src(s_img, path);

    // 检查是否加载成功 (LVGL 9: 如果 src 是文件路径, 会尝试解码)
    // 直接设置源为文件路径
    lv_img_set_src(s_img, path);

    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    s_img_valid = true;

    // 更新信息
    char buf[128];
    char display[NAME_LEN];
    strlcpy(display, s_images[idx], sizeof(display));
    char *dot = strrchr(display, '.');
    if (dot) *dot = 0;
    snprintf(buf, sizeof(buf), "%s\n第 %d/%d 张", display, idx + 1, s_count);
    lv_label_set_text(s_info, buf);
    lv_label_set_text(s_hint, "上=上一张  下=下一张");
}

static void update_ui(void) {
    if (!s_scr) return;
    if (s_count == 0) {
        lv_label_set_text(s_info, "无图片\n请通过蓝牙传输\nBMP/JPG/PNG 文件");
        lv_label_set_text(s_hint, "OK=刷新  长按=返回");
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    show_image(s_sel);
}

void demo_image_enter(void) {
    s_scr = ui_pixel_screen_create("图片浏览");

    s_img = lv_img_create(s_scr);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_size(s_img, 240, 280);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_info = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_info, &notosanssc_16, 0);
    lv_obj_set_pos(s_info, 10, 285);
    lv_obj_set_size(s_info, 220, 30);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_pos(s_hint, 10, 305);
    lv_obj_set_size(s_hint, 220, 14);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_scr);

    scan_images();
    update_ui();
}

void demo_image_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL; s_img = NULL; s_info = NULL; s_hint = NULL;
    }
}

void demo_image_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        if (s_sel > 0) { s_sel--; update_ui(); }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_sel < s_count - 1) { s_sel++; update_ui(); }
    } else if (btn == BSP_BTN_OK) {
        scan_images();
        update_ui();
    }
}
