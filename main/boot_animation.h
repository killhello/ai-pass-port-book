// 开机动画播放模块
// 从 SPIFFS 读取 RGB565 格式的帧文件，直接绘制到 LCD 面板
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 播放开机动画
// - 从 SPIFFS 的 /boot_anim/ 目录读取帧
// - 按索引文件指定的帧率播放
// - 按任意键跳过（如果已初始化按键）
// - 返回: true=播放完成, false=被跳过或失败
bool boot_animation_play(void);
