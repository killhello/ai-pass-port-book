// 开机动画 - 从 SPIFFS 读取 RGB565 帧并播放(支持 RLE 压缩帧)
// 帧文件格式: [u16 LE flag] + payload
//   flag=0: 原始 RGB565(大端) 240x320
//   flag=1: RLE 流,每项 4 字节 = [u16 BE 像素数][u16 BE RGB565 颜色]
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

// RLE 流式解码状态:游程可跨 chunk 边界
typedef struct {
    FILE    *f;
    int      mode;        // 0=raw 1=rle
    uint16_t run;         // 当前游程剩余像素数
    uint8_t  color[2];    // 当前游程颜色(保持文件内大端字节序)
    bool     eof;
} anim_dec_t;

static bool dec_open(anim_dec_t *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->f = fopen(path, "rb");
    if (!d->f) return false;
    uint8_t flag[2];
    if (fread(flag, 1, 2, d->f) != 2) { fclose(d->f); return false; }
    d->mode = flag[0] | (flag[1] << 8);   // LE u16
    return true;
}

static void dec_close(anim_dec_t *d) {
    if (d->f) fclose(d->f);
    d->f = NULL;
}

// 读取 npix 个像素到 out(大端字节序,与原始帧格式一致)。返回实际像素数。
static size_t dec_read(anim_dec_t *d, uint8_t *out, size_t npix) {
    size_t done = 0;
    while (done < npix) {
        if (d->mode == 0) {                       // raw
            size_t n = fread(out + done * 2, 2, npix - done, d->f);
            return done + n;
        }
        if (d->run == 0) {                        // 取下一个游程头
            uint8_t hdr[4];
            if (fread(hdr, 1, 4, d->f) != 4) { d->eof = true; return done; }
            d->run = (uint16_t)((hdr[0] << 8) | hdr[1]);
            d->color[0] = hdr[2];
            d->color[1] = hdr[3];
        }
        size_t n = d->run < (npix - done) ? d->run : (npix - done);
        for (size_t i = 0; i < n; i++) {
            out[(done + i) * 2]     = d->color[0];
            out[(done + i) * 2 + 1] = d->color[1];
        }
        d->run -= (uint16_t)n;
        done += n;
    }
    return done;
}

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
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/boot_anim/boot_%03d.bin", i);

        anim_dec_t dec;
        if (!dec_open(&dec, path)) {
            ESP_LOGW(TAG, "无法打开 %s", path);
            continue;
        }

        for (int y = 0; y < SCREEN_H; y += CHUNK_ROWS) {
            int rows = (y + CHUNK_ROWS <= SCREEN_H) ? CHUNK_ROWS : (SCREEN_H - y);
            size_t need = (size_t)SCREEN_W * rows * 2;
            size_t got = dec_read(&dec, s_buf, need / 2) * 2;
            if (got != need) {
                ESP_LOGW(TAG, "%s 读取不足: 需要 %zu 得到 %zu", path, need, got);
                break;
            }
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_W, y + rows, s_buf);
        }
        dec_close(&dec);

        if (i == 0) ESP_LOGI(TAG, "第一帧已显示: %s", path);
        vTaskDelay(pdMS_TO_TICKS(77));  // 13fps
    }

    ESP_LOGI(TAG, "=== 开机动画结束 ===");
    return true;
}
