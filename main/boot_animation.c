// 开机动画 - 用小缓冲逐行填充,不依赖 SPIFFS,不依赖 malloc
#include "boot_animation.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "boot_anim";

#define SCREEN_W 240
#define SCREEN_H 320
#define ROW_SIZE (SCREEN_W * 2)  // 一行 480 字节

static uint8_t s_row[ROW_SIZE];  // 静态分配,不用 malloc

static void fill_color(uint16_t rgb565_be_hi, uint16_t rgb565_be_lo) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) {
        ESP_LOGE(TAG, "panel NULL");
        return;
    }
    for (int i = 0; i < ROW_SIZE; i += 2) {
        s_row[i] = rgb565_be_hi;
        s_row[i+1] = rgb565_be_lo;
    }
    for (int y = 0; y < SCREEN_H; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + 1, s_row);
    }
}

bool boot_animation_play(void) {
    ESP_LOGI(TAG, "=== 开机动画开始 ===");

    esp_lcd_panel_handle_t panel = bsp_display_panel();
    ESP_LOGI(TAG, "panel=%p", panel);
    if (!panel) {
        ESP_LOGE(TAG, "显示面板未初始化,跳过开机动画");
        return false;
    }

    ESP_LOGI(TAG, "红色...");
    fill_color(0xF8, 0x00);  // R5=11111 G6=000000 B5=00000
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "绿色...");
    fill_color(0x07, 0xE0);  // R5=00000 G6=111111 B5=00000
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "蓝色...");
    fill_color(0x00, 0x1F);  // R5=00000 G6=000000 B5=11111
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "白色...");
    memset(s_row, 0xFF, ROW_SIZE);
    for (int y = 0; y < SCREEN_H; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + 1, s_row);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "=== 开机动画结束 ===");
    return true;
}
