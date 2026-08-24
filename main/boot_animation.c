// 开机动画 - 先用代码生成测试画面,不依赖 SPIFFS
#include "boot_animation.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
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

    uint8_t *buf = malloc(FRAME_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "分配缓冲失败");
        return false;
    }

    // 测试 1: 纯红色 (RGB565: R=0xF800)
    ESP_LOGI(TAG, "显示红色...");
    for (int i = 0; i < FRAME_SIZE; i += 2) {
        buf[i] = 0xF8;   // R5
        buf[i+1] = 0x00;  // G6+B5
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, buf);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 测试 2: 纯绿色 (RGB565: G=0x07E0)
    ESP_LOGI(TAG, "显示绿色...");
    for (int i = 0; i < FRAME_SIZE; i += 2) {
        buf[i] = 0x07;
        buf[i+1] = 0xE0;
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, buf);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 测试 3: 纯蓝色 (RGB565: B=0x001F)
    ESP_LOGI(TAG, "显示蓝色...");
    for (int i = 0; i < FRAME_SIZE; i += 2) {
        buf[i] = 0x00;
        buf[i+1] = 0x1F;
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, buf);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 测试 4: 白色
    ESP_LOGI(TAG, "显示白色...");
    memset(buf, 0xFF, FRAME_SIZE);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_W, SCREEN_H, buf);
    vTaskDelay(pdMS_TO_TICKS(2000));

    free(buf);
    ESP_LOGI(TAG, "颜色测试完成");
    return true;
}
