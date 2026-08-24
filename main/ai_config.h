// main/ai_config.h —— WiFi 与 AI 接口配置。
// ★ 本仓库若为 public,密钥会随源码公开,请将仓库设为 Private 或改用 CI Secrets。
#pragma once

// --- WiFi(2.4GHz) ---
#define WIFI_SSID     "luo2.4g"
#define WIFI_PASS     "5886721luo"
#define WIFI_CONNECT_TIMEOUT_MS  20000   // 连接超时,超时返回失败

// --- AI 接口(OpenAI 兼容 /chat/completions) ---
#define AI_API_URL    "https://uuapi.io/v1/chat/completions"
#define AI_API_KEY    "sk-d5f7913ccde1434a681f08cf295b3465199841eb661158afd077c6f27bf6ba40"
#define AI_MODEL      "gpt-6"

#define AI_HTTP_TIMEOUT_MS       30000  // 请求超时
#define AI_RESP_MAX_BYTES        65536  // 响应体累积上限
#define AI_WORKER_STACK_SIZE     16384  // TLS 握手需要较大栈
