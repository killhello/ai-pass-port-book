// main/captive_portal.c —— 热点配网：AP + DNS 劫持 + HTTP 配网页面
// 手机连上 ESP32 热点后自动弹出配网页面（或手动访问 192.168.4.1）
#include "captive_portal.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "captive";
static const char *AP_SSID = "ESP-WiFi";
static const char *NVS_NS = "wifi_sta";

static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static volatile bool s_running = false;
static captive_portal_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

// ===== HTML 配网页面 =====
static const char PAGE_HTML[] = "<!DOCTYPE html>"
"<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi 配网</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Microsoft YaHei',sans-serif;"
"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;"
"display:flex;align-items:center;justify-content:center;padding:20px}"
".card{background:#fff;border-radius:16px;padding:28px;max-width:360px;width:100%;"
"box-shadow:0 20px 60px rgba(0,0,0,.2)}"
"h1{text-align:center;color:#333;margin-bottom:6px;font-size:22px}"
".sub{text-align:center;color:#888;margin-bottom:20px;font-size:13px}"
".item{background:#f5f5f5;border:1px solid #e0e0e0;border-radius:10px;padding:14px 16px;"
"margin-bottom:10px;cursor:pointer;transition:all .2s}"
".item:active,.item.on{background:#e8eaff;border-color:#667eea}"
".name{font-weight:600;color:#333;font-size:15px}"
".info{font-size:12px;color:#999;margin-top:2px}"
"label{display:block;margin-bottom:5px;font-weight:500;color:#333;font-size:14px}"
"input{width:100%;padding:12px;border:1px solid #ddd;border-radius:8px;font-size:16px;"
"outline:none;margin-bottom:14px;transition:border .2s}"
"input:focus{border-color:#667eea}"
"button{width:100%;padding:14px;border:none;border-radius:8px;font-size:16px;"
"font-weight:600;cursor:pointer;color:#fff;"
"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);margin-bottom:8px}"
"button:disabled{opacity:.5;cursor:not-allowed}"
"button.sec{background:#f1f3f4;color:#333}"
".msg{text-align:center;padding:12px;border-radius:8px;margin-top:12px;display:none}"
".msg.ok{display:block;background:#e8f5e9;color:#2e7d32}"
".msg.err{display:block;background:#ffebee;color:#c62828}"
".hide{display:none!important}"
"#loading{text-align:center;padding:30px;color:#888}"
"</style></head><body><div class='card'>"
"<h1>\xF0\x9F\x93\x81 WiFi 配网</h1>"
"<p class='sub'>选择网络并输入密码</p>"
"<div id='scan-btn'><button onclick='scan()'>扫描附近网络</button></div>"
"<div id='loading' class='hide'>扫描中...</div>"
"<div id='list'></div>"
"<div id='form' class='hide'>"
"<label>WiFi 名称</label><input id='ssid' placeholder='自动填入或手动输入'>"
"<label>WiFi 密码</label><input id='pass' type='password' placeholder='留空=开放网络'>"
"<button onclick='connect()'>连接</button>"
"<button class='sec' onclick='scan()'>重新扫描</button>"
"</div>"
"<div id='msg' class='msg'></div>"
"</div><script>"
"let sel='';"
"function scan(){"
"document.getElementById('list').innerHTML='';"
"document.getElementById('form').classList.add('hide');"
"document.getElementById('loading').classList.remove('hide');"
"document.getElementById('scan-btn').classList.add('hide');"
"fetch('/api/scan').then(r=>r.json()).then(d=>{"
"document.getElementById('loading').classList.add('hide');"
"document.getElementById('scan-btn').classList.remove('hide');"
"let h='';d.forEach((a,i)=>{"
"h+='<div class=\"item\" onclick=\"pick('+i+')\" id=\"ap'+i+'\">'"
"+'<div class=\"name\">'+(a.locked?'\\xF0\\x9F\\x94\\x12 ':'')+a.ssid+'</div>'"
"+'<div class=\"info\">'+a.rssi+'dBm  ch'+a.ch+'</div></div>';"
"});document.getElementById('list').innerHTML=h;"
"}).catch(()=>{document.getElementById('loading').classList.add('hide');"
"document.getElementById('scan-btn').classList.remove('hide');});}"
"function pick(i){document.querySelectorAll('.item').forEach(e=>e.classList.remove('on'));"
"let el=document.getElementById('ap'+i);if(el)el.classList.add('on');"
"fetch('/api/scan').then(r=>r.json()).then(d=>{"
"document.getElementById('ssid').value=d[i].ssid;"
"document.getElementById('form').classList.remove('hide');sel=d[i].ssid;});}"
"function connect(){"
"let s=document.getElementById('ssid').value.trim();"
"let p=document.getElementById('pass').value;"
"if(!s){alert('请输入WiFi名称');return;}"
"let m=document.getElementById('msg');m.className='msg';m.textContent='正在连接...';"
"fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:s,pass:p})}).then(r=>r.json()).then(d=>{"
"if(d.ok){m.className='msg ok';m.textContent='连接成功!设备将自动重启';}"
"else{m.className='msg err';m.textContent='连接失败: '+(d.err||'未知错误');}}"
"}).catch(()=>{m.className='msg err';m.textContent='请求失败';});}"
"</script></body></html>";

// ===== DNS 服务器（劫持所有请求到 192.168.4.1） =====
static void dns_task(void *param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    uint8_t buf[256];
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;

        // 构造 DNS 响应：把任何域名指向 192.168.4.1
        uint8_t resp[256];
        memcpy(resp, buf, len);
        // 标志位：QR=1, OPCODE=0, AA=1
        resp[2] = 0x85;
        resp[3] = 0x80;
        // 回答数 = 1
        resp[6] = 0; resp[7] = 1;
        // 追加回答：A 记录
        int offset = len;
        // 名字指针
        resp[offset++] = 0xC0;
        resp[offset++] = 0x0C;
        // 类型 A, 类 IN
        resp[offset++] = 0x00; resp[offset++] = 0x01;  // A
        resp[offset++] = 0x00; resp[offset++] = 0x01;  // IN
        // TTL 60s
        resp[offset++] = 0x00; resp[offset++] = 0x00;
        resp[offset++] = 0x00; resp[offset++] = 0x3C;
        // 数据长度 4
        resp[offset++] = 0x00; resp[offset++] = 0x04;
        // IP: 192.168.4.1
        resp[offset++] = 192; resp[offset++] = 168;
        resp[offset++] = 4;   resp[offset++] = 1;

        sendto(sock, resp, offset, 0, (struct sockaddr *)&client_addr, addr_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ===== HTTP 处理 =====
static int handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static int handle_scan(httpd_req_t *req) {
    // 扫描 WiFi
    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t ap_cnt = 0;
    esp_wifi_scan_get_ap_num(&ap_cnt);
    uint16_t fetch = ap_cnt < 15 ? ap_cnt : 15;
    wifi_ap_record_t *rec = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!rec) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&fetch, rec);

    // 构造 JSON
    char json[2048];
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < fetch; i++) {
        if (rec[i].ssid[0] == '\0') continue;
        const char *lock = (rec[i].authmode != WIFI_AUTH_OPEN) ? "true" : "false";
        off += snprintf(json + off, sizeof(json) - off,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"lock\":%s}",
            i > 0 ? "," : "", rec[i].ssid, rec[i].rssi, rec[i].primary, lock);
        if (off >= sizeof(json) - 128) break;
    }
    off += snprintf(json + off, sizeof(json) - off, "]");
    free(rec);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

// 解析 JSON 字段（极简版）
static int json_get(const char *json, const char *key, char *out, int out_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
        out[i] = '\0';
        return i;
    }
    return -1;
}

static int handle_connect(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"空请求\"}");
    }
    buf[len] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    json_get(buf, "ssid", ssid, sizeof(ssid));
    json_get(buf, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"SSID为空\"}");
    }

    ESP_LOGI(TAG, "收到配网: SSID=%s", ssid);

    // 保存到 NVS
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        if (pass[0]) nvs_set_str(h, "pass", pass);
        else nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // 延迟后重启连接
    vTaskDelay(pdMS_TO_TICKS(500));
    if (s_user_cb) s_user_cb(true, s_user_data);
    return ESP_OK;
}

// Captive portal 检测重定向
static int handle_204(httpd_req_t *req) {
    return httpd_resp_send(req, NULL, 0);
}
static int handle_hotspot(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}
static int handle_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// ===== 启动/停止 =====
esp_err_t captive_portal_start(captive_portal_cb_t cb, void *user) {
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_user_cb = cb;
    s_user_data = user;

    // 确保 WiFi 初始化
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_ALREADY_STARTED) {
        ESP_LOGE(TAG, "WiFi init 失败: %s", esp_err_to_name(err));
        return err;
    }

    // AP+STA 模式（AP 用于配网，STA 用于扫描）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // AP 配置
    wifi_config_t ap_cfg = {0};
    strcpy((char *)ap_cfg.ap.ssid, AP_SSID);
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // 启动 WiFi
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start 失败: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    // 创建 DNS 任务
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);

    // 创建 HTTP 服务器
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.stack_size = 8192;
    err = httpd_start(&s_httpd, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 注册路由
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    httpd_uri_t uri_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = handle_scan };
    httpd_uri_t uri_connect = { .uri = "/api/connect", .method = HTTP_POST, .handler = handle_connect };
    httpd_uri_t uri_204 = { .uri = "/gen_204", .method = HTTP_GET, .handler = handle_204 };
    httpd_uri_t uri_hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_hotspot };
    httpd_uri_t uri_redirect = { .uri = "/redirect", .method = HTTP_GET, .handler = handle_redirect };
    httpd_uri_t uri_connecttest = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_204 };
    httpd_uri_t uri_ncsi = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = handle_204 };

    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_connect);
    httpd_register_uri_handler(s_httpd, &uri_204);
    httpd_register_uri_handler(s_httpd, &uri_hotspot);
    httpd_register_uri_handler(s_httpd, &uri_redirect);
    httpd_register_uri_handler(s_httpd, &uri_connecttest);
    httpd_register_uri_handler(s_httpd, &uri_ncsi);

    s_running = true;
    ESP_LOGI(TAG, "热点配网启动: SSID=%s, 访问 http://192.168.4.1", AP_SSID);
    return ESP_OK;
}

esp_err_t captive_portal_stop(void) {
    if (!s_running) return ESP_OK;

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "热点配网已停止");
    return ESP_OK;
}

bool captive_portal_is_running(void) {
    return s_running;
}
