// 手机配网：开热点「AI为什么100-xxxx」（不加密，只在设置时开），手机连上后自动弹出设置页
// （DNS 把所有域名都指向 192.168.4.1，手机以为要「登录 WLAN」）。填 WiFi 和接入方式，保存后重启。
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "app.h"

static const char *TAG = "portal";
#define PORTAL_TIMEOUT_MS (15 * 60 * 1000)
#define MAX_APS 20

static char s_aps[MAX_APS][33];
static int s_naps;

// ================= DNS：所有 A 记录都回 192.168.4.1 =================
static void dns_task(void *arg) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
  if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
    ESP_LOGE(TAG, "DNS 启动失败");
    vTaskDelete(NULL);
  }
  uint8_t buf[512];
  for (;;) {
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
    if (n < 12) continue;
    uint16_t qd = buf[4] << 8 | buf[5];
    if ((buf[2] & 0x80) || qd != 1) continue;  // 只回答单个查询
    // 找到问题段结尾：名字 + 类型(2) + 类(2)
    int p = 12;
    while (p < n && buf[p]) p += buf[p] + 1;
    p += 5;
    if (p > n) continue;
    uint16_t qtype = buf[p - 4] << 8 | buf[p - 3];
    buf[2] = 0x81; buf[3] = 0x80;               // 响应、递归可用、无错误
    buf[6] = 0; buf[7] = qtype == 1 ? 1 : 0;    // 只给 A 记录回答案
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    n = p;
    if (qtype == 1) {
      const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
      memcpy(buf + n, ans, sizeof ans);
      n += sizeof ans;
    }
    sendto(sock, buf, n, 0, (struct sockaddr *)&from, fl);
  }
}

// ================= 网页 =================
static void url_decode(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char h[3] = { s[1], s[2], 0 };
      *o++ = (char)strtol(h, NULL, 16);
      s += 2;
    } else *o++ = *s;
  }
  *o = 0;
}

/** 从 a=1&b=2 里取一个字段（解码后） */
static void field(const char *body, const char *key, char *out, size_t max) {
  out[0] = 0;
  if (httpd_query_key_value(body, key, out, max) == ESP_OK) url_decode(out);
}

static void send_esc(httpd_req_t *req, const char *s) {
  char buf[8];
  for (; *s; s++) {
    const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
    if (e) httpd_resp_sendstr_chunk(req, e);
    else { buf[0] = *s; buf[1] = 0; httpd_resp_sendstr_chunk(req, buf); }
  }
}

static const char *HEAD =
  "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>AI为什么100 设置</title><style>"
  ":root{--bg:#fff8ef;--card:#fff;--ink:#2b2118;--sub:#8a7a6a;--accent:#ff8a3d;--line:#eadccb}"
  "@media(prefers-color-scheme:dark){:root{--bg:#1c1712;--card:#29221b;--ink:#f6ece2;--sub:#b3a291;--line:#3a3027}}"
  "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 -apple-system,'PingFang SC',sans-serif}"
  "main{max-width:480px;margin:0 auto;padding:20px 16px 40px}h1{font-size:22px;margin:0 0 4px}.sub{color:var(--sub);font-size:13px;margin:0 0 16px}"
  ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:14px}"
  "label{display:block;font-size:13px;color:var(--sub);margin:10px 0 4px}label:first-child{margin-top:0}"
  "input[type=text],input[type=password]{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:10px;background:transparent;color:var(--ink);font:inherit}"
  ".mode label{display:flex;gap:8px;align-items:flex-start;color:var(--ink);font-size:15px;margin:8px 0}.mode small{display:block;color:var(--sub);font-size:12px}"
  "button{width:100%;padding:14px;border:0;border-radius:999px;background:var(--accent);color:#fff;font-size:17px}"
  ".hint{font-size:12px;color:var(--sub);margin-top:6px}a{color:var(--accent)}"
  "</style></head><body><main>";

static esp_err_t page_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr_chunk(req, HEAD);
  httpd_resp_sendstr_chunk(req,
    "<h1>AI为什么100 · 设置</h1><p class=sub>填好后设备会重新启动。孩子按住按钮问「为什么」，几分钟后就能在手机上看到科普动画。</p>"
    "<form method=post action=/save><div class=card><label>WiFi 名称（只支持 2.4G）</label><input type=text name=ssid list=aps required value=\"");
  send_esc(req, g_cfg.ssid);
  httpd_resp_sendstr_chunk(req, "\"><datalist id=aps>");
  for (int i = 0; i < s_naps; i++) {
    httpd_resp_sendstr_chunk(req, "<option value=\"");
    send_esc(req, s_aps[i]);
    httpd_resp_sendstr_chunk(req, "\">");
  }
  httpd_resp_sendstr_chunk(req, "</datalist><label>WiFi 密码</label><input type=password name=pass autocomplete=off placeholder=\"");
  httpd_resp_sendstr_chunk(req, g_cfg.pass[0] ? "已保存；WiFi 名称不变时留空 = 不改" : "没有密码就留空");
  httpd_resp_sendstr_chunk(req, "\"></div><div class='card mode'>"
    "<label><input type=radio name=mode value=direct onchange=m()");
  httpd_resp_sendstr_chunk(req, settings_direct() ? " checked" : "");
  httpd_resp_sendstr_chunk(req, "><span>直连 AI为什么100<small>个人使用：在 aiwhy100.com/app 「设置 → API Key」创建一个 Key 填在下面</small></span></label>"
    "<label><input type=radio name=mode value=gateway onchange=m()");
  httpd_resp_sendstr_chunk(req, settings_direct() ? "" : " checked");
  httpd_resp_sendstr_chunk(req, "><span>自建网关<small>合作厂商 / 自己部署了 device-why100 网关</small></span></label>"
    "<div id=d><label>API Key</label><input type=password name=key autocomplete=off placeholder=\"");
  httpd_resp_sendstr_chunk(req, g_cfg.key[0] ? "已保存；留空 = 不改" : "svf_live_…");
  httpd_resp_sendstr_chunk(req, "\"></div><div id=g><label>网关地址</label><input type=text name=server placeholder=\"http://192.168.1.10:8080\" value=\"");
  if (!settings_direct()) send_esc(req, g_cfg.server);
  httpd_resp_sendstr_chunk(req, "\"><label>设备令牌（网关设了 DEVICE_TOKENS 才填）</label><input type=password name=token autocomplete=off placeholder=\"");
  httpd_resp_sendstr_chunk(req, g_cfg.token[0] ? "已保存；留空 = 不改" : "可以不填");
  httpd_resp_sendstr_chunk(req, "\"></div></div><button>保存并重启</button>"
    "<p class=hint>录音只用来识别成文字，识别完就丢，不保存。视频由 AI 生成。</p></form>"
    "<script>function m(){var d=document.querySelector('input[value=direct]').checked;"
    "document.getElementById('d').style.display=d?'':'none';document.getElementById('g').style.display=d?'none':''}m()</script>"
    "</main></body></html>");
  return httpd_resp_sendstr_chunk(req, NULL);
}

static void restart_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(800));
  say("saved");
  esp_restart();
}

static esp_err_t save_post(httpd_req_t *req) {
  static char body[1024];
  int len = req->content_len < (int)sizeof body - 1 ? req->content_len : (int)sizeof body - 1, got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, body + got, len - got);
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = 0;

  settings_t s = g_cfg;
  char buf[160];
  field(body, "ssid", buf, sizeof buf);
  strlcpy(s.ssid, buf, sizeof s.ssid);
  field(body, "pass", buf, sizeof buf);
  // WiFi 名称没变、密码留空：沿用原来的密码
  if (buf[0] || strcmp(s.ssid, g_cfg.ssid) != 0) strlcpy(s.pass, buf, sizeof s.pass);
  field(body, "mode", buf, sizeof buf);
  strlcpy(s.mode, strcmp(buf, "gateway") == 0 ? "gateway" : "direct", sizeof s.mode);
  field(body, "key", buf, sizeof buf);
  if (buf[0]) strlcpy(s.key, buf, sizeof s.key);
  field(body, "server", buf, sizeof buf);
  strlcpy(s.server, strcmp(s.mode, "gateway") == 0 ? buf : "", sizeof s.server);
  field(body, "token", buf, sizeof buf);
  if (buf[0]) strlcpy(s.token, buf, sizeof s.token);

  const char *err = !s.ssid[0] ? "请填 WiFi 名称"
                  : strcmp(s.mode, "direct") == 0 && !s.key[0] ? "请填 API Key"
                  : strcmp(s.mode, "gateway") == 0 && strncmp(s.server, "http", 4) != 0 ? "网关地址要以 http:// 或 https:// 开头"
                  : NULL;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, HEAD);
  if (err) {
    httpd_resp_sendstr_chunk(req, "<h1>还差一点</h1><div class=card>");
    httpd_resp_sendstr_chunk(req, err);
    httpd_resp_sendstr_chunk(req, "</div><a href=/>‹ 返回修改</a></main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
  }
  esp_err_t e = settings_save(&s);
  httpd_resp_sendstr_chunk(req, e == ESP_OK
    ? "<h1>设置好啦</h1><div class=card>设备正在重启，大约 10 秒后会说「我准备好啦」。<br><br>"
      "如果它说「连不上网」，多半是 WiFi 名称或密码不对（只支持 2.4G），重新连上这个热点再设置一次。</div></main></body></html>"
    : "<h1>保存失败</h1><div class=card>请重试。</div></main></body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
  if (e == ESP_OK) {
    ESP_LOGI(TAG, "已保存：WiFi「%s」，方式 %s", s.ssid, s.mode);
    xTaskCreate(restart_task, "restart", 4096, NULL, 5, NULL);
  }
  return ESP_OK;
}

/** 手机的联网检测（/generate_204、/hotspot-detect.html……）全部跳到设置页，触发「登录 WLAN」弹窗 */
static esp_err_t redirect_get(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, NULL, 0);
}

// ================= 入口 =================
static void scan(void) {
  wifi_scan_config_t sc = { .show_hidden = false };
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
  uint16_t n = MAX_APS;
  wifi_ap_record_t recs[MAX_APS];
  esp_wifi_scan_get_ap_records(&n, recs);
  s_naps = 0;
  for (int i = 0; i < n; i++) {
    const char *ssid = (const char *)recs[i].ssid;
    if (!ssid[0]) continue;
    bool dup = false;
    for (int k = 0; k < s_naps; k++) dup |= strcmp(s_aps[k], ssid) == 0;
    if (!dup) strlcpy(s_aps[s_naps++], ssid, sizeof s_aps[0]);
  }
  ESP_LOGI(TAG, "附近 %d 个 WiFi", s_naps);
}

void portal_run(void) {
  g_state = ST_SETUP;
  esp_wifi_stop();
  esp_netif_create_default_wifi_ap();
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
  int n = snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "AI为什么100-%02X%02X", mac[4], mac[5]);
  ap.ap.ssid_len = n;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  // STA 不连任何网络，只用来扫描附近的 WiFi
  wifi_config_t sta = { 0 };
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  ESP_ERROR_CHECK(esp_wifi_start());
  scan();
  ESP_LOGI(TAG, "配网热点「%s」已开启，手机连上后打开 http://192.168.4.1/", (char *)ap.ap.ssid);

  xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL);
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 8192;
  httpd_handle_t srv = NULL;
  ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
  const httpd_uri_t uris[] = {
    { .uri = "/", .method = HTTP_GET, .handler = page_get },
    { .uri = "/save", .method = HTTP_POST, .handler = save_post },
    { .uri = "/*", .method = HTTP_GET, .handler = redirect_get },
  };
  for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);

  say("setup");
  int64_t t0 = esp_timer_get_time();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if ((esp_timer_get_time() - t0) / 1000 > PORTAL_TIMEOUT_MS) {
      ESP_LOGW(TAG, "15 分钟没有保存，重启");
      esp_restart();
    }
  }
}
