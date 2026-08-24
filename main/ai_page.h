// main/ai_page.h —— AI 对话页(双击 OK 触发)。
#pragma once

#include <stdbool.h>
#include "ai_chat.h"

// 创建 AI 屏并显示"思考中",然后发起一次 "你好" 请求。
// 仅在主菜单状态调用(内部会加 LVGL 锁)。在途请求未完成时调用无效。
void ai_page_open(void);

// 关闭 AI 屏并回到主菜单(内部会加 LVGL 锁)。
void ai_page_close(void);

bool ai_page_is_open(void);
