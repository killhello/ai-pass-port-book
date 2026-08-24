// 开机动画 - 从 SPIFFS 读取 RGB565 帧并播放
#include "boot_animation.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "boot_anim";

#define SCREEN_W 240
#define SCREEN_H 320
#define CHUNK_ROWS 20
#define CHUNK_SIZE (SCREEN_W * CHUNK_ROWS * 2)

static uint8_t s_buf[CHUNK_SIZE];

static int count_frames(void) {
    DIR *dir = opendir("/spiffs/boot_anim");
    if (!dir) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strstr(ent->d_name, "boot_") && strstr(ent->d_name, ".bin"))
            count++;
    }
    closedir(dir);
    return count;
}

bool boot_animation_play(void) {
    ESP_LOGI(TAG, "=== 开机动画开始 ===");

    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) {
        ESP_LOGE(TAG, "panel NULL, 跳过");
        return false;
    }

    int frame_count = count_frames();
    ESP_LOGI(TAG, "找到 %d 帧", frame_count);
    if (frame_count == 0) {
        ESP_LOGW(TAG, "没有开机动画帧,跳过");
        return false;
    }

    for (int i = 0; i < frame_count; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);

        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "无法打开 %s", path);
            continue;
        }

        for (int y = 0; y < SCREEN_H; y += CHUNK_ROWS) {
            int rows = (y + CHUNK_ROWS <= SCREEN_H) ? CHUNK_ROWS : (SCREEN_H - y);
            size_t need = (size_t)SCREEN_W * rows * 2;
            size_t got = fread(s_buf, 1, need, f);
            if (got != need) {
                ESP_LOGW(TAG, "%s 读取不足: 需要 %zu 得到 %zu", path, need, got);
                break;
            }
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + rows, s_buf);
        }
        fclose(f);

        if (i == 0) ESP_LOGI(TAG, "第一帧已显示: %s", path);
        vTaskDelay(pdMS_TO_TICKS(100));  // 10fps
    }

    ESP_LOGI(TAG, "=== 开机动画结束 ===");
    return true;
}
