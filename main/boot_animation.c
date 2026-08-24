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

#define SCREEN_W 240
#define SCREEN_H 320
#define FRAME_SIZE (SCREEN_W * SCREEN_H * 2)

bool boot_animation_play(void) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) {
        ESP_LOGE(TAG, "显示面板未初始化");
        return false;
    }

    // 检查 SPIFFS 是否已挂载
    size_t total = 0, used = 0;
    esp_err_t spiffs_info = esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS info: ret=%s, total=%d, used=%d", esp_err_to_name(spiffs_info), (int)total, (int)used);

    // 尝试打开第一帧
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_000.bin");
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "无法打开 %s, 跳过开机动画", path);
        return false;
    }
    ESP_LOGI(TAG, "成功打开 %s", path);

    // 分配帧缓冲
    uint8_t *frame_buf = malloc(FRAME_SIZE);
    if (!frame_buf) {
        ESP_LOGE(TAG, "分配帧缓冲失败 (%d 字节)", FRAME_SIZE);
        fclose(f);
        return false;
    }

    // 读取第一帧
    size_t n = fread(frame_buf, 1, FRAME_SIZE, f);
    fclose(f);
    ESP_LOGI(TAG, "读取帧数据: %d / %d 字节", (int)n, FRAME_SIZE);
    if (n != FRAME_SIZE) {
        ESP_LOGW(TAG, "帧数据不完整");
        free(frame_buf);
        return false;
    }

    // 显示第一帧,停留 3 秒观察
    ESP_LOGI(TAG, "显示第一帧...");
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, frame_buf);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 尝试播放剩余帧
    for (int i = 1; i < 28; i++) {
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);
        FILE *f2 = fopen(path, "rb");
        if (!f2) {
            ESP_LOGW(TAG, "帧 %d 打不开,停止", i);
            break;
        }
        fread(frame_buf, 1, FRAME_SIZE, f2);
        fclose(f2);
        esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, frame_buf);
        vTaskDelay(pdMS_TO_TICKS(100));  // 10fps
    }

    free(frame_buf);
    ESP_LOGI(TAG, "开机动画完成");
    return true;
}
