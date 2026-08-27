// main/demo_music.c —— 蓝牙音乐播放页面。
// UP/DOWN=选择歌曲, OK=播放/暂停, 长按 OK=返回菜单
#include "demo.h"
#include "audio_player.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_cn_16.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "demo_music";
#define MUSIC_DIR "/spiffs/music"
#define MAX_SONGS 32
#define NAME_LEN 64

static lv_obj_t *s_scr;
static lv_obj_t *s_title_label;
static lv_obj_t *s_info;
static lv_obj_t *s_hint;
static char s_songs[MAX_SONGS][NAME_LEN];
static int s_song_count = 0;
static int s_sel = 0;
static int s_playing = -1;

static void scan_songs(void) {
    s_song_count = 0;
    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_song_count < MAX_SONGS) {
        if (ent->d_type == DT_REG) {
            const char *name = ent->d_name;
            size_t nlen = strlen(name);
            if (nlen > 4 && strcmp(name + nlen - 4, ".wav") == 0) {
                strlcpy(s_songs[s_song_count], name, NAME_LEN);
                s_song_count++;
            }
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描到 %d 首歌曲", s_song_count);
}

static void update_ui(void) {
    if (!s_scr) return;

    player_state_t st = audio_player_get_state();
    const char *state_str = "";
    switch (st) {
    case PLAYER_IDLE:    state_str = "就绪"; break;
    case PLAYER_PLAYING: state_str = "播放中"; break;
    case PLAYER_PAUSED:  state_str = "已暂停"; break;
    case PLAYER_ERROR:   state_str = "播放错误"; break;
    }

    if (s_song_count == 0) {
        lv_label_set_text(s_title_label, "蓝牙音乐");
        lv_label_set_text(s_info, "无音频文件\n请通过蓝牙传输\nWAV 文件");
        lv_label_set_text(s_hint, "OK=刷新  长按=返回");
        return;
    }

    char buf[128];
    const char *name = (s_sel >= 0 && s_sel < s_song_count) ? s_songs[s_sel] : "";
    char display[NAME_LEN];
    strlcpy(display, name, sizeof(display));
    char *dot = strrchr(display, '.');
    if (dot) *dot = 0;

    snprintf(buf, sizeof(buf), "%s", display);
    lv_label_set_text(s_title_label, buf);

    uint32_t pos = audio_player_get_position_ms() / 1000;
    uint32_t dur = audio_player_get_duration_ms() / 1000;
    snprintf(buf, sizeof(buf), "%s\n%02lu:%02lu / %02lu:%02lu\n第 %d/%d 首",
             state_str, (unsigned long)(pos/60), (unsigned long)(pos%60),
             (unsigned long)(dur/60), (unsigned long)(dur%60),
             s_sel + 1, s_song_count);
    lv_label_set_text(s_info, buf);

    if (st == PLAYER_PLAYING) {
        lv_label_set_text(s_hint, "OK=暂停  UP/DOWN=切歌");
    } else {
        lv_label_set_text(s_hint, "OK=播放  UP/DOWN=选歌");
    }
}

static void on_player_state(player_state_t state) {
    if (bsp_lvgl_lock(200)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

static void on_player_progress(uint32_t pos_ms, uint32_t total_ms) {
    if (bsp_lvgl_lock(100)) {
        update_ui();
        bsp_lvgl_unlock();
    }
}

void demo_music_enter(void) {
    s_scr = ui_pixel_screen_create("蓝牙音乐");

    s_title_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_title_label, &notosanssc_16, 0);
    lv_obj_set_pos(s_title_label, 50, 14);
    lv_obj_set_width(s_title_label, 185);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 48, 220, 240, UI_PAPER);

    scan_songs();

    s_info = lv_label_create(panel);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_font(s_info, &notosanssc_16, 0);
    lv_obj_set_size(s_info, 204, 180);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);

    // 提示
    s_hint = lv_label_create(panel);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_hint, &notosanssc_16, 0);
    lv_obj_set_size(s_hint, 204, 20);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_scr);

    audio_player_init();
    audio_player_set_state_cb(on_player_state);
    audio_player_set_progress_cb(on_player_progress);

    update_ui();
}

void demo_music_exit(void) {
    audio_player_set_state_cb(NULL);
    audio_player_set_progress_cb(NULL);
    audio_player_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL; s_info = NULL; s_hint = NULL; s_title_label = NULL;
    }
}

void demo_music_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        player_state_t st = audio_player_get_state();
        if (s_song_count == 0) {
            scan_songs();
            update_ui();
            return;
        }
        if (st == PLAYER_PLAYING) {
            audio_player_stop();
        } else {
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, s_songs[s_sel]);
            audio_player_play(path);
        }
        update_ui();
    } else if (btn == BSP_BTN_UP) {
        if (s_sel > 0) { s_sel--; update_ui(); }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_sel < s_song_count - 1) { s_sel++; update_ui(); }
    }
}
