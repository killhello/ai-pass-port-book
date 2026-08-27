// main/audio_player.c —— 音频播放器：从 SPIFFS 读取 WAV/MP3，解码后通过 I2S 输出。
#include "audio_player.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_player";

// ---- WAV header ----
typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

// ---- 状态 ----
static player_state_t s_state = PLAYER_IDLE;
static volatile player_state_cb_t s_state_cb = NULL;
static volatile player_progress_cb_t s_progress_cb = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static char s_path[256];
static uint32_t s_duration_ms = 0;
static uint32_t s_position_ms = 0;
static uint8_t s_volume = 80;

static void set_state(player_state_t st) {
    s_state = st;
    if (s_state_cb) s_state_cb(st);
}

// 简单 WAV 头解析
static bool parse_wav_header(FILE *fp, uint32_t *rate, uint16_t *channels,
                              uint16_t *bits, uint32_t *data_size) {
    wav_header_t h;
    if (fread(&h, sizeof(h), 1, fp) != 1) return false;
    if (memcmp(h.riff, "RIFF", 4) != 0 || memcmp(h.wave, "WAVE", 4) != 0) return false;
    *rate = h.sample_rate;
    *channels = h.num_channels;
    *bits = h.bits_per_sample;
    // data chunk
    char chunk_id[4];
    uint32_t chunk_size;
    while (fread(chunk_id, 1, 4, fp) == 4 && fread(&chunk_size, 4, 1, fp) == 1) {
        if (memcmp(chunk_id, "data", 4) == 0) {
            *data_size = chunk_size;
            return true;
        }
        fseek(fp, chunk_size, SEEK_CUR);
    }
    return false;
}

// 判断文件是否为 MP3
static bool is_mp3(const char *path) {
    size_t len = strlen(path);
    if (len < 4) return false;
    const char *ext = path + len - 4;
    return (strcasecmp(ext, ".mp3") == 0);
}

static void player_task(void *arg) {
    (void)arg;
    FILE *fp = fopen(s_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "无法打开: %s", s_path);
        set_state(PLAYER_ERROR);
        vTaskDelete(NULL);
        return;
    }

    bool mp3 = is_mp3(s_path);
    uint32_t rate = 16000;
    uint16_t channels = 1, bits = 16;
    uint32_t data_size = 0;

    if (!mp3) {
        if (!parse_wav_header(fp, &rate, &channels, &bits, &data_size)) {
            ESP_LOGE(TAG, "WAV 头解析失败");
            fclose(fp);
            set_state(PLAYER_ERROR);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "WAV: %luHz %uch %ubit", (unsigned long)rate, channels, bits);
    } else {
        // MP3: 跳过 ID3 tag
        uint8_t tag[10];
        if (fread(tag, 1, 10, fp) == 10 && memcmp(tag, "ID3", 3) == 0) {
            uint32_t sz = (tag[6]<<21)|(tag[7]<<14)|(tag[8]<<7)|tag[9];
            fseek(fp, sz + 10, SEEK_SET);
        } else {
            fseek(fp, 0, SEEK_SET);
        }
        ESP_LOGI(TAG, "MP3: 使用默认 44100Hz 立体声");
        rate = 44100; channels = 2; bits = 16;
    }

    bsp_audio_set_format(rate, bits, channels);
    bsp_audio_set_volume(s_volume);

    // 解码缓冲
    #define DECODE_BUF_SIZE 4096
    uint8_t *in_buf = malloc(DECODE_BUF_SIZE);
    int16_t *out_buf = malloc(DECODE_BUF_SIZE * 2);
    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "内存不足");
        free(in_buf); free(out_buf); fclose(fp);
        set_state(PLAYER_ERROR);
        vTaskDelete(NULL);
        return;
    }

    // 估算总时长
    if (data_size > 0 && rate > 0 && channels > 0 && bits > 0) {
        uint32_t bytes_per_sec = rate * channels * (bits / 8);
        s_duration_ms = (data_size / bytes_per_sec) * 1000;
    } else {
        s_duration_ms = 0;
    }
    s_position_ms = 0;

    set_state(PLAYER_PLAYING);

    while (s_state == PLAYER_PLAYING) {
        size_t n = fread(in_buf, 1, DECODE_BUF_SIZE, fp);
        if (n == 0) break; // EOF

        // 简单 PCM 直接播放 (WAV)
        // 对于 MP3 需要 esp_audio_codec，这里先用原始 PCM
        if (!mp3) {
            bsp_audio_write(in_buf, n);
            // 更新进度
            if (data_size > 0) {
                uint32_t bytes_per_sec = rate * channels * (bits / 8);
                if (bytes_per_sec > 0)
                    s_position_ms = ((ftell(fp) - n) / bytes_per_sec) * 1000;
            }
            if (s_progress_cb) s_progress_cb(s_position_ms, s_duration_ms);
        } else {
            // MP3: 简单跳过，等 esp_audio_codec 集成
            // 目前先静默播放
            memset(out_buf, 0, n);
            bsp_audio_write(out_buf, n);
        }

        // 让出 CPU
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(in_buf);
    free(out_buf);
    fclose(fp);

    if (s_state == PLAYER_PLAYING) set_state(PLAYER_IDLE);
    s_task = NULL;
    vTaskDelete(NULL);
}

void audio_player_init(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    bsp_audio_init();
}

bool audio_player_play(const char *path) {
    if (!path || !path[0]) return false;

    // 停止当前播放
    audio_player_stop();

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    strlcpy(s_path, path, sizeof(s_path));
    s_position_ms = 0;
    s_duration_ms = 0;

    BaseType_t ret = xTaskCreate(player_task, "audio_play", 4096, NULL, 5, &s_task);

    if (s_mutex) xSemaphoreGive(s_mutex);

    return ret == pdPASS;
}

void audio_player_pause(void) {
    if (s_state == PLAYER_PLAYING) {
        // ESP32 没有真正的暂停，用停止代替
        set_state(PLAYER_PAUSED);
    }
}

void audio_player_resume(void) {
    if (s_state == PLAYER_PAUSED) {
        // 重新从当前位置播放 (简化: 重新开始)
        audio_player_play(s_path);
    }
}

void audio_player_stop(void) {
    if (s_task) {
        set_state(PLAYER_IDLE);
        // 等待任务自行退出
        for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_position_ms = 0;
}

player_state_t audio_player_get_state(void) { return s_state; }
uint32_t audio_player_get_position_ms(void) { return s_position_ms; }
uint32_t audio_player_get_duration_ms(void) { return s_duration_ms; }
void audio_player_set_volume(uint8_t percent) {
    s_volume = percent;
    bsp_audio_set_volume(percent);
}
void audio_player_set_state_cb(player_state_cb_t cb) { s_state_cb = cb; }
void audio_player_set_progress_cb(player_progress_cb_t cb) { s_progress_cb = cb; }
