// main/ai_chat.c —— HTTPS POST 到 OpenAI 兼容接口并解析回复。
// 流程:worker 任务里 WiFi 连接 -> TLS POST -> 累积响应 -> cJSON 取
// choices[0].message.content -> 回调。全程不碰 LVGL。
#include "ai_chat.h"
#include "ai_config.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ai_chat";

typedef struct {
    char        *prompt;
    ai_result_cb_t cb;
    char        *body;        // 响应体累积缓冲
    size_t       len;
    size_t       cap;
    bool         failed;
} ai_req_t;

static volatile bool s_busy;

static bool body_append(ai_req_t *r, const char *data, int len) {
    if (r->len + len + 1 > AI_RESP_MAX_BYTES) {
        ESP_LOGW(TAG, "响应超过上限,截断");
        return true;                       // 继续收但不再扩容
    }
    if (r->len + len + 1 > r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8192;
        while (ncap < r->len + len + 1) ncap *= 2;
        char *nb = realloc(r->body, ncap);
        if (!nb) { r->failed = true; return false; }
        r->body = nb;
        r->cap = ncap;
    }
    memcpy(r->body + r->len, data, len);
    r->len += len;
    r->body[r->len] = '\0';
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *evt) {
    ai_req_t *r = (ai_req_t *)evt->user_data;
    if (!r) return ESP_OK;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!r->failed && evt->data_len > 0) {
            if (!body_append(r, (const char *)evt->data, evt->data_len)) {
                return ESP_FAIL;           // 内存不足,中断
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// 从响应 JSON 里提取 choices[0].message.content。成功返回 malloc 的字符串。
static char *extract_content(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "响应不是合法 JSON: %.120s", body);
        return NULL;
    }
    char *out = NULL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first   = cJSON_GetArrayItem(choices, 0);
    cJSON *msg     = cJSON_GetObjectItem(first, "message");
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        out = strdup(content->valuestring);
    }
    if (!out) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *emsg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(emsg)) out = strdup(emsg->valuestring);
    }
    cJSON_Delete(root);
    return out;
}

static void ai_worker(void *arg) {
    ai_req_t *r = (ai_req_t *)arg;

    // 1. WiFi
    if (wifi_sta_connect() != ESP_OK) {
        r->cb(AI_STATE_FAIL, "WiFi 连接失败");
        goto done;
    }

    // 2. 请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", AI_MODEL);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", r->prompt);
    cJSON_AddItemToArray(msgs, m);
    char *post = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post) {
        r->cb(AI_STATE_FAIL, "构造请求失败");
        goto done;
    }

    // 3. HTTPS POST
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", AI_API_KEY);

    esp_http_client_config_t cfg = {
        .url = AI_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .timeout_ms = AI_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        r->cb(AI_STATE_FAIL, "HTTP 初始化失败");
        free(post);
        goto done;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_post_field(cli, post, strlen(post));

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    free(post);

    ESP_LOGI(TAG, "HTTP done: %s, status=%d, body=%u bytes",
             esp_err_to_name(err), status, (unsigned)r->len);

    // 4. 解析
    if (err == ESP_OK && status >= 200 && status < 300 && r->body) {
        char *content = extract_content(r->body);
        if (content) {
            r->cb(AI_STATE_OK, content);
            free(content);
        } else {
            r->cb(AI_STATE_FAIL, "回复解析失败");
        }
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "请求失败 status=%d", status);
        r->cb(AI_STATE_FAIL, msg);
    }

done:
    free(r->prompt);
    free(r->body);
    free(r);
    s_busy = false;
    vTaskDelete(NULL);
}

bool ai_chat_busy(void) {
    return s_busy;
}

bool ai_chat_request_async(const char *prompt, ai_result_cb_t cb) {
    if (s_busy) return false;
    ai_req_t *r = calloc(1, sizeof(*r));
    if (!r) return false;
    r->prompt = strdup(prompt);
    r->cb = cb;
    if (!r->prompt) { free(r); return false; }

    s_busy = true;
    BaseType_t ok = xTaskCreate(ai_worker, "ai_chat", AI_WORKER_STACK_SIZE, r,
                                5, NULL);
    if (ok != pdPASS) {
        s_busy = false;
        free(r->prompt);
        free(r);
        return false;
    }
    return true;
}
