// main/ble_ebook.h —— BLE 整书传输：手机发送完整 TXT 文件，设备存储到 SPIFFS。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 传输状态
typedef enum {
    BLE_EBOOK_IDLE = 0,       // 空闲，等待连接
    BLE_EBOOK_CONNECTED,      // 手机已连接
    BLE_EBOOK_RECEIVING,      // 正在接收文件
    BLE_EBOOK_DONE,           // 传输完成
    BLE_EBOOK_ERROR,          // 传输错误
} ble_ebook_state_t;

// 回调：状态变化
typedef void (*ble_ebook_state_cb_t)(ble_ebook_state_t state);

// 回调：传输进度
typedef void (*ble_ebook_progress_cb_t)(uint32_t received, uint32_t total);

// 启动 BLE 电子书传输服务
bool ble_ebook_start(void);

// 停止
void ble_ebook_stop(void);

bool ble_ebook_is_running(void);
ble_ebook_state_t ble_ebook_get_state(void);

// 获取当前接收的文件名
const char *ble_ebook_get_filename(void);

// 获取已接收字节数
uint32_t ble_ebook_get_received(void);

// 获取文件总大小（由手机端告知）
uint32_t ble_ebook_get_filesize(void);

// 注册回调
void ble_ebook_set_state_cb(ble_ebook_state_cb_t cb);
void ble_ebook_set_progress_cb(ble_ebook_progress_cb_t cb);
