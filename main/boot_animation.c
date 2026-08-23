// 开机动画播放 - 直接操作 LCD 面板，速度最快
#include "boot_animation.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "boot_anim";

// 屏幕尺寸
#define SCREEN_W 240
#define SCREEN_H 320
#define FRAME_SIZE (SCREEN_W * SCREEN_H * 2)  // RGB565 = 2 bytes per pixel

// 帧索引文件结构
typedef struct {
    uint16_t frame_count;
    uint16_t fps;
    uint32_t frame_size;
} anim_index_t;

// 读取帧索引
static bool read_index(const char *path, anim_index_t *idx) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(idx, 1, sizeof(anim_index_t), f);
    fclose(f);
    return n == sizeof(anim_index_t);
}

// 读取单帧数据到缓冲区
static bool read_frame(const char *path, uint8_t *buf, size_t buf_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, buf_size, f);
    fclose(f);
    return n == buf_size;
}

bool boot_animation_play(void) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) {
        ESP_LOGE(TAG, "显示面板未初始化");
        return false;
    }

    // 读取索引
    anim_index_t idx;
    if (!read_index("/spiffs/boot_anim/frame_index.bin", &idx)) {
        ESP_LOGW(TAG, "找不到动画索引文件，跳过开机动画");
        return false;
    }

    ESP_LOGI(TAG, "开机动画: %d 帧, %d fps, 每帧 %d 字节",
             idx.frame_count, idx.fps, idx.frame_size);

    if (idx.frame_size != FRAME_SIZE) {
        ESP_LOGW(TAG, "帧大小不匹配 (期望 %d, 实际 %d)，跳过", FRAME_SIZE, idx.frame_size);
        return false;
    }

    // 分配帧缓冲（用 malloc，用完释放）
    uint8_t *frame_buf = malloc(FRAME_SIZE);
    if (!frame_buf) {
        ESP_LOGE(TAG, "分配帧缓冲失败");
        return false;
    }

    // 计算每帧间隔 (ms)
    uint32_t frame_delay_ms = 1000 / idx.fps;
    bool skipped = false;

    // 逐帧播放
    for (int i = 0; i < idx.frame_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);

        if (!read_frame(path, frame_buf, FRAME_SIZE)) {
            ESP_LOGW(TAG, "读取帧 %d 失败", i);
            break;
        }

        // 绘制到屏幕
        esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, frame_buf);

        // 延时控制帧率
        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }

    free(frame_buf);
    ESP_LOGI(TAG, "开机动画结束 (%s)", skipped ? "跳过" : "完成");
    return !skipped;
}
