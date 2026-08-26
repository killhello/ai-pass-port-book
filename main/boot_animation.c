// 开机动画 - 从 SPIFFS 读取 RGB565 帧并播放(支持 RLE 压缩帧)
// 帧文件格式: [u16 LE flag] + payload
//   flag=0: 原始 RGB565(大端) 240x320
//   flag=1: RLE 流,每项 4 字节 = [u16 BE 像素数][u16 BE RGB565 颜色]
//
// 性能设计:
//   - 目标帧率固定 80fps, 实际速率受 SPI 线速限制, 预算耗尽即全速播放
//   - 40 行大块传输: 每帧仅 8 次 draw_bitmap, 降低命令开销
//   - 双缓冲 ping-pong: DMA 传输 A 块时解码写入 B 块
//   - 缓冲用完即释放(heap_caps), 不占用 LVGL 阶段的常驻内存
#include "boot_animation.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "boot_anim";

#define SCREEN_W 240
#define SCREEN_H 320
#define CHUNK_ROWS 40                     // 8 次传完整帧
#define CHUNK_BYTES (SCREEN_W * CHUNK_ROWS * 2)

typedef struct {
    FILE    *f;
    int      mode;
    uint16_t run;
    uint8_t  color[2];
    bool     eof;
} anim_dec_t;

static bool dec_open(anim_dec_t *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->f = fopen(path, "rb");
    if (!d->f) return false;
    uint8_t flag[2];
    if (fread(flag, 1, 2, d->f) != 2) { fclose(d->f); d->f = NULL; return false; }
    d->mode = flag[0] | (flag[1] << 8);
    return true;
}

static void dec_close(anim_dec_t *d) {
    if (d->f) fclose(d->f);
    d->f = NULL;
}

static inline void fill_color(uint8_t *out, uint8_t c0, uint8_t c1, size_t npix) {
    size_t bytes = npix * 2;
    size_t i = 0;
    for (; i + 1 < bytes; i += 2) {
        out[i]     = c0;
        out[i + 1] = c1;
    }
    if (i < bytes) out[i] = c0;
}

static size_t dec_read(anim_dec_t *d, uint8_t *out, size_t npix) {
    size_t done = 0;
    while (done < npix) {
        if (d->mode == 0) {
            size_t n = fread(out + done * 2, 2, npix - done, d->f);
            return done + n;
        }
        if (d->run == 0) {
            uint8_t hdr[4];
            if (fread(hdr, 1, 4, d->f) != 4) { d->eof = true; return done; }
            d->run = (uint16_t)((hdr[0] << 8) | hdr[1]);
            d->color[0] = hdr[2];
            d->color[1] = hdr[3];
        }
        size_t n = d->run < (npix - done) ? d->run : (npix - done);
        fill_color(out + done * 2, d->color[0], d->color[1], n);
        d->run -= (uint16_t)n;
        done += n;
    }
    return done;
}

static int read_frame_count(void) {
    FILE *f = fopen("/spiffs/boot_anim/frame_index.bin", "rb");
    if (!f) return 0;
    uint8_t buf[8];
    size_t n = fread(buf, 1, 8, f);
    fclose(f);
    if (n < 8) return 0;
    return buf[0] | (buf[1] << 8);
}

bool boot_animation_play(void) {
    ESP_LOGI(TAG, "=== 开机动画开始 ===");

    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) { ESP_LOGE(TAG, "panel NULL"); return false; }

    const int frame_count = read_frame_count();
    if (frame_count == 0) { ESP_LOGW(TAG, "无帧"); return false; }

    // 双缓冲: DMA 发送 A 时解码 B。动画阶段独占 ~38KB, 结束即还堆。
    uint8_t *buf[2] = {0};
    for (int i = 0; i < 2; i++) {
        buf[i] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!buf[i]) {
            ESP_LOGE(TAG, "缓冲 %d 分配失败", i);
            while (i--) free(buf[i]);
            return false;
        }
    }

    const TickType_t budget = pdMS_TO_TICKS(1000 / BOOT_ANIM_FPS);

    for (int i = 0; i < frame_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);

        anim_dec_t dec;
        if (!dec_open(&dec, path)) continue;

        uint32_t t0 = xTaskGetTickCount();
        int cur = 0;

        for (int y = 0; y < SCREEN_H && dec.eof == false; y += CHUNK_ROWS) {
            if (dec_read(&dec, buf[cur], SCREEN_W * CHUNK_ROWS)
                    != SCREEN_W * CHUNK_ROWS) break;
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W,
                                      y + CHUNK_ROWS, buf[cur]);
            cur = 1 - cur;
        }

        dec_close(&dec);

        TickType_t elapsed = xTaskGetTickCount() - t0;
        if (elapsed < budget) vTaskDelay(budget - elapsed);
    }

    free(buf[0]); free(buf[1]);
    ESP_LOGI(TAG, "=== 开机动画结束 ===");
    return true;
}
