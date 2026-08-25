// main/ai_config.h —— AI 接口配置(WiFi 由用户在设备上选择)。
#pragma once

// --- WiFi ---
#define WIFI_SSID     ""   // 不再硬编码,由用户在 WiFi 设置页选择
#define WIFI_PASS     ""
#define WIFI_CONNECT_TIMEOUT_MS  20000

// --- AI 接口(OpenAI 兼容 /chat/completions) ---
#define AI_API_URL    "https://uuapi.io/v1/chat/completions"
#define AI_API_KEY    "sk-d5f7913ccde1434a681f08cf295b3465199841eb661158afd077c6f27bf6ba40"
#define AI_MODEL      "gpt-6"

#define AI_HTTP_TIMEOUT_MS       30000  // 请求超时
#define AI_RESP_MAX_BYTES        65536  // 响应体累积上限
#define AI_WORKER_STACK_SIZE     16384  // TLS 握手需要较大栈
