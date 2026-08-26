// main/ble_ebook.h —— BLE 电子书流式传输服务。
// 手机通过 BLE 发送文件路径和页面内容，设备按需请求页码。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// BLE 电子书服务状态
typedef enum {
    BLE_EBOOK_IDLE = 0,       // 空闲，等待连接
    BLE_EBOOK_CONNECTED,      // 手机已连接
    BLE_EBOOK_READY,          // 手机已发送文件路径
    BLE_EBOOK_STREAMING,      // 正在传输页面内容
} ble_ebook_state_t;

// 回调：状态变化通知
typedef void (*ble_ebook_state_cb_t)(ble_ebook_state_t state);

// 回调：收到页面内容
typedef void (*ble_ebook_page_cb_t)(const char *content, uint32_t page_num);

// 回调：收到文件信息
typedef void (*ble_ebook_file_cb_t)(const char *filename, uint32_t total_pages);

// 启动 BLE 电子书服务
// 服务 UUID: A0E80001-2B4D-4C9A-B5C1-9E3D6F0A5B21
// 特征:
//   0x10 = 文件路径 (手机→设备, Write)
//   0x20 = 页码请求 (设备→手机, Notify)
//   0x30 = 内容数据 (手机→设备, Write)
//   0x40 = 控制命令 (双向, Read/Write)
bool ble_ebook_start(void);

// 停止 BLE 电子书服务
void ble_ebook_stop(void);

// 检查是否正在运行
bool ble_ebook_is_running(void);

// 获取当前状态
ble_ebook_state_t ble_ebook_get_state(void);

// 设备端请求指定页码（手机收到通知后发送内容）
// page_num: 页码（从 0 开始）
bool ble_ebook_request_page(uint32_t page_num);

// 获取当前文件名（从路径中提取）
const char *ble_ebook_get_filename(void);

// 获取当前页码
uint32_t ble_ebook_get_current_page(void);

// 获取总页数
uint32_t ble_ebook_get_total_pages(void);

// 获取当前页内容
const char *ble_ebook_get_page_content(void);

// 注册状态变化回调
void ble_ebook_set_state_cb(ble_ebook_state_cb_t cb);

// 注册页面内容回调
void ble_ebook_set_page_cb(ble_ebook_page_cb_t cb);

// 注册文件信息回调
void ble_ebook_set_file_cb(ble_ebook_file_cb_t cb);
