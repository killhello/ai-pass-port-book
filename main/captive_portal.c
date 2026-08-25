// main/captive_portal.c —— 热点配网：AP + HTTP 配网页面
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
static volatile bool s_running = false;
static captive_portal_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

// ===== HTML 配网页面（精简版） =====
static const char PAGE_HTML[] = "<!DOCTYPE html>"
"<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".c{background:#fff;border-radius:16px;padding:24px;max-width:340px;width:100%;box-shadow:0 20px 60px rgba(0,0,0,.2)}"
"h1{text-align:center;color:#333;margin-bottom:16px;font-size:20px}"
".i{background:#f5f5f5;border:1px solid #e0e0e0;border-radius:8px;padding:12px;margin-bottom:8px;cursor:pointer}"
".i:active,.i.on{background:#e8eaff;border-color:#667eea}"
".n{font-weight:600;color:#333}.r{font-size:12px;color:#999}"
"input{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:15px;margin:6px 0 12px}"
"input:focus{outline:none;border-color:#667eea}"
"button{width:100%;padding:12px;border:none;border-radius:6px;font-size:15px;font-weight:600;cursor:pointer;color:#fff;background:linear-gradient(135deg,#667eea,#764ba2);margin-bottom:6px}"
".s{text-align:center;padding:8px;border-radius:6px;margin-top:8px;display:none;font-size:14px}"
".ok{display:block;background:#e8f5e9;color:#2e7d32}"
".er{display:block;background:#ffebee;color:#c62828}"
".h{display:none!important}"
"</style></head><body><div class='c'>"
"<h1>WiFi 配网</h1>"
"<div id='sb'><button onclick='sc()'>扫描网络</button></div>"
"<div id='ld' class='h' style='text-align:center;padding:20px;color:#888'>扫描中...</div>"
"<div id='ls'></div>"
"<div id='fm' class='h'>"
"<input id='ssid' placeholder='WiFi 名称'>"
"<input id='pass' type='password' placeholder='密码 (留空=开放)'>"
"<button onclick='cn()'>连接</button>"
"<button style='background:#f1f3f4;color:#333' onclick='sc()'>重新扫描</button>"
"</div>"
"<div id='mg' class='s'></div>"
"</div><script>"
"function sc(){"
"document.getElementById('ls').innerHTML='';"
"document.getElementById('fm').classList.add('h');"
"document.getElementById('ld').classList.remove('h');"
"document.getElementById('sb').classList.add('h');"
"fetch('/scan').then(r=>r.json()).then(d=>{"
"document.getElementById('ld').classList.add('h');"
"document.getElementById('sb').classList.remove('h');"
"let h='';d.forEach((a,i)=>{"
"h+='<div class=\"i\" onclick=\"pk('+i+')\" id=\"a'+i+'\">'"
"+'<div class=\"n\">'+a.ssid+'</div>'"
"+'<div class=\"r\">'+a.rssi+'dBm</div></div>';"
"});document.getElementById('ls').innerHTML=h;})"
".catch(()=>{document.getElementById('ld').classList.add('h');"
"document.getElementById('sb').classList.remove('h');});}"
"function pk(i){document.querySelectorAll('.i').forEach(e=>e.classList.remove('on'));"
"let e=document.getElementById('a'+i);if(e)e.classList.add('on');"
"fetch('/scan').then(r=>r.json()).then(d=>{"
"document.getElementById('ssid').value=d[i].ssid;"
"document.getElementById('fm').classList.remove('h');});}"
"function cn(){"
"let s=document.getElementById('ssid').value.trim();"
"let p=document.getElementById('pass').value;"
"if(!s){alert('请输入WiFi名称');return;}"
"let m=document.getElementById('mg');m.className='s';m.textContent='连接中...';"
"fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:s,pass:p})}).then(r=>r.json()).then(d=>{"
"if(d.ok){m.className='s ok';m.textContent='成功!设备将重启';}"
"else{m.className='s er';m.textContent='失败: '+(d.err||'');}}"
").catch(()=>{m.className='s er';m.textContent='请求失败';});}"
"</script></body></html>";

// ===== HTTP 处理 =====
static int handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static int handle_scan(httpd_req_t *req) {
    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t ap_cnt = 0;
    esp_wifi_scan_get_ap_num(&ap_cnt);
    uint16_t fetch = ap_cnt < 12 ? ap_cnt : 12;
    wifi_ap_record_t *rec = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!rec) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&fetch, rec);

    char json[1536];
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < fetch; i++) {
        if (rec[i].ssid[0] == '\0') continue;
        off += snprintf(json + off, sizeof(json) - off,
            "%s{\"ssid\":\"%s\",\"rssi\":%d}",
            (i > 0 && off > 1) ? "," : "", rec[i].ssid, rec[i].rssi);
        if (off >= sizeof(json) - 80) break;
    }
    off += snprintf(json + off, sizeof(json) - off, "]");
    free(rec);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

static int json_get_str(const char *json, const char *key, char *out, int out_sz) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
    out[i] = '\0';
    return i;
}

static int handle_connect(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"empty\"}");
    }
    buf[len] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    json_get_str(buf, "ssid", ssid, sizeof(ssid));
    json_get_str(buf, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"ssid empty\"}");
    }

    ESP_LOGI(TAG, "收到配网: SSID=%s", ssid);

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

    vTaskDelay(pdMS_TO_TICKS(500));
    if (s_user_cb) s_user_cb(true, s_user_data);
    return ESP_OK;
}

// Captive portal 检测 — 全部重定向到首页
static int handle_captive(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t captive_portal_start(captive_portal_cb_t cb, void *user) {
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_user_cb = cb;
    s_user_data = user;

    // 设置 AP+STA 模式
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
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start 失败: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    // HTTP 服务器（不开 DNS，靠 HTTP 重定向做 captive portal）
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 6;
    http_cfg.stack_size = 4096;
    err = httpd_start(&s_httpd, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 注册路由
    httpd_uri_t uri;
    uri.method = HTTP_GET;
    uri.handler = handle_captive;

    uri.uri = "/"; httpd_register_uri_handler(s_httpd, &uri);

    uri.uri = "/scan";
    uri.handler = handle_scan;
    httpd_register_uri_handler(s_httpd, &uri);

    uri.uri = "/connect";
    uri.method = HTTP_POST;
    uri.handler = handle_connect;
    httpd_register_uri_handler(s_httpd, &uri);

    uri.method = HTTP_GET;
    uri.uri = "/gen_204"; uri.handler = handle_captive;
    httpd_register_uri_handler(s_httpd, &uri);

    uri.uri = "/hotspot-detect.html"; uri.handler = handle_captive;
    httpd_register_uri_handler(s_httpd, &uri);

    uri.uri = "/connecttest.txt"; uri.handler = handle_captive;
    httpd_register_uri_handler(s_httpd, &uri);

    s_running = true;
    ESP_LOGI(TAG, "热点配网启动: SSID=%s", AP_SSID);
    return ESP_OK;
}

esp_err_t captive_portal_stop(void) {
    if (!s_running) return ESP_OK;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "热点配网已停止");
    return ESP_OK;
}

bool captive_portal_is_running(void) {
    return s_running;
}
