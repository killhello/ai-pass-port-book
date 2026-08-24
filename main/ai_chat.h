// main/ai_chat.h —— AI 请求(OpenAI 兼容 /chat/completions)。
#pragma once

#include <stdbool.h>

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_CONNECTING,   // 正在连 WiFi/发请求
    AI_STATE_OK,           // text = 模型回复
    AI_STATE_FAIL,         // text = 失败原因(简短)
} ai_state_t;

// 结果回调:运行在 ai_chat worker 任务上下文!
// 回调里若要动 LVGL,必须自己 bsp_lvgl_lock();不要阻塞太久。
typedef void (*ai_result_cb_t)(ai_state_t state, const char *text);

// 异步发起一次请求。内部创建一次性 worker 任务,完成后自删。
// 同时只允许一个在途请求:在途时调用返回 false。
bool ai_chat_request_async(const char *prompt, ai_result_cb_t cb);

// 是否有在途请求。
bool ai_chat_busy(void);
