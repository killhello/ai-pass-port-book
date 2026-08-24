// 开机动画 - 从 SPIFFS 读取 RGB565 帧并播放(支持 RLE 压缩帧)
// 帧文件格式: [u16 LE flag] + payload
//   flag=0: 原始 RGB565(大端) 240x320
//   flag=1: RLE 流,每项 4 字节 = [u16 BE 像素数][u16 BE RGB565 颜色]
//
// 优化:
//   - 双缓冲 ping-pong: DMA 传输 A 缓冲时解码写入 B 缓冲,消除撕裂
//   - fill_color 批量填充: 替代逐像素循环
//   - frame_index.bin 读帧数/帧率: 避免 opendir 目录扫描
//   - 精确帧率控制: 补偿解码+传输耗时
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
#define CHUNK_PIXELS (SCREEN_W * CHUNK_ROWS)

static uint8_t s_buf[2][CHUNK_PIXELS * 2];

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

static int read_frame_index(int *out_fps) {
    FILE *f = fopen("/spiffs/boot_anim/frame_index.bin", "rb");
    if (!f) return 0;
    uint8_t buf[8];
    size_t n = fread(buf, 1, 8, f);
    fclose(f);
    if (n < 8) return 0;
    if (out_fps) *out_fps = buf[2] | (buf[3] << 8);
    return buf[0] | (buf[1] << 8);
}

static int count_frames_dir(void) {
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
    if (!panel) { ESP_LOGE(TAG, "panel NULL"); return false; }

    int fps = 13;
    int frame_count = read_frame_index(&fps);
    if (frame_count == 0) frame_count = count_frames_dir();
    ESP_LOGI(TAG, "%d 帧, %d fps", frame_count, fps);
    if (frame_count == 0) { ESP_LOGW(TAG, "无帧"); return false; }

    int ms_per_frame = 1000 / fps;

    for (int i = 0; i < frame_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);

        anim_dec_t dec;
        if (!dec_open(&dec, path)) {
            ESP_LOGW(TAG, "无法打开 %s", path);
            continue;
        }

        uint32_t tick_start = xTaskGetTickCount();
        int cur = 0;

        for (int y = 0; y < SCREEN_H; y += CHUNK_ROWS) {
            int rows = (y + CHUNK_ROWS <= SCREEN_H) ? CHUNK_ROWS : (SCREEN_H - y);
            size_t need = (size_t)SCREEN_W * rows;
            size_t got = dec_read(&dec, s_buf[cur], need);
            if (got != need) {
                ESP_LOGW(TAG, "%s 行%d 不足: %zu/%zu", path, y, got, need);
                break;
            }
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + rows, s_buf[cur]);
            cur = 1 - cur;
        }

        dec_close(&dec);

        if (i == 0) ESP_LOGI(TAG, "第一帧已显示");

        uint32_t elapsed = (xTaskGetTickCount() - tick_start) * portTICK_PERIOD_MS;
        if (elapsed < (uint32_t)ms_per_frame) {
            vTaskDelay(pdMS_TO_TICKS(ms_per_frame - elapsed));
        }
    }

    ESP_LOGI(TAG, "=== 开机动画结束 ===");
    return true;
}
