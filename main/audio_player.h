// main/audio_player.h —— 音频播放器：从 SPIFFS 读取 WAV/MP3，解码后通过 I2S 输出。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ERROR,
} player_state_t;

typedef void (*player_state_cb_t)(player_state_t state);
typedef void (*player_progress_cb_t)(uint32_t pos_ms, uint32_t total_ms);

// 初始化播放器
void audio_player_init(void);

// 播放文件
bool audio_player_play(const char *path);

// 暂停/恢复
void audio_player_pause(void);
void audio_player_resume(void);

// 停止
void audio_player_stop(void);

// 获取状态
player_state_t audio_player_get_state(void);
uint32_t audio_player_get_position_ms(void);
uint32_t audio_player_get_duration_ms(void);

// 音量 0-100
void audio_player_set_volume(uint8_t percent);

// 回调
void audio_player_set_state_cb(player_state_cb_t cb);
void audio_player_set_progress_cb(player_progress_cb_t cb);
