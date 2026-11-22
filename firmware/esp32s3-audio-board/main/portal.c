// 手机配网（无屏设备，所以每一步都要有声音 / 页面反馈）：
//   开热点「AI为什么100-xxxx」（不加密，只在设置时开）→ 语音「请连上 AI为什么100 开头的网络」，没人连每 2 分钟提醒一次
//   手机连上 → 语音「手机连上我啦，请在弹出的页面里……」；DNS 把所有域名指向 192.168.4.1，手机自动弹出设置页
//   点「保存并连接」→ 语音「收到，我来试试连上网」，页面显示进度；当场用填的 WiFi 连一次、用填的 Key（或网关）调一次接口
//   → 成功：语音「设置好啦」，保存并重启；失败：页面和语音都说清楚原因，不保存，改了再试
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "app.h"

static const char *TAG = "portal";
#define PORTAL_TIMEOUT_MS (15 * 60 * 1000)
#define REMIND_MS         (2 * 60 * 1000)
#define MAX_REMINDS       5
#define MAX_APS           20
#define PAGE_MAX          (24 * 1024)

typedef struct { char ssid[33]; int8_t rssi; bool open; } ap_t;
static ap_t s_aps[MAX_APS];
static int s_naps;
static char s_mac[18];
volatile bool g_portal;

// ---------- 状态 ----------
typedef enum { P_IDLE, P_CONNECTING, P_CHECKING, P_OK, P_FAIL } pstate_t;
static const char *PSTATE[] = { "idle", "connecting", "checking", "ok", "fail" };
static volatile pstate_t s_pstate = P_IDLE;
static char s_pmsg[200];
static settings_t s_pending;
static EventGroupHandle_t s_ev;
static volatile int s_reason;
static volatile int s_phones;          // 连着热点的手机数
static volatile bool s_phone_joined;   // 有手机刚连上（主循环里播提示）
#define EV_GOT_IP BIT0
#define EV_FAILED BIT1

static void set_status(pstate_t st, const char *msg) {
  strlcpy(s_pmsg, msg, sizeof s_pmsg);
  s_pstate = st;
  ESP_LOGI(TAG, "状态 %s：%s", PSTATE[st], msg);
}
static bool busy(void) { return s_pstate == P_CONNECTING || s_pstate == P_CHECKING; }

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
    int p = 12;                                 // 问题段：名字 + 类型(2) + 类(2)
    while (p < n && buf[p]) p += buf[p] + 1;
    p += 5;
    if (p > n) continue;
    uint16_t qtype = buf[p - 4] << 8 | buf[p - 3];
    buf[2] = 0x81; buf[3] = 0x80;
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

// ================= WiFi 事件 =================
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    xEventGroupSetBits(s_ev, EV_FAILED);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_ev, EV_GOT_IP);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    s_phones++;
    s_phone_joined = true;
    ESP_LOGI(TAG, "手机连上热点（%d 台）", s_phones);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    if (s_phones > 0) s_phones--;
    ESP_LOGI(TAG, "手机断开热点（%d 台）", s_phones);
  }
}

// ================= 验证：连 WiFi → 调一次接口 → 保存重启 =================
static const char *wifi_reason_text(int r) {
  switch (r) {
    case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE: case WIFI_REASON_AUTH_EXPIRE:
      return "WiFi 密码不对，请检查一下（区分大小写）";
    case WIFI_REASON_NO_AP_FOUND: case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "找不到这个 WiFi：板子只支持 2.4G，请确认名称没写错、路由器离得不远";
    default: return NULL;
  }
}

static void verify_task(void *arg) {
  char msg[200];
  g_state = ST_THINK;
  say("connecting");

  // 1. 连 WiFi（最多试 3 次；密码不对就不再重试）
  wifi_config_t sta = { 0 };
  strlcpy((char *)sta.sta.ssid, s_pending.ssid, sizeof sta.sta.ssid);
  strlcpy((char *)sta.sta.password, s_pending.pass, sizeof sta.sta.password);
  sta.sta.threshold.authmode = s_pending.pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
  esp_wifi_disconnect();
  esp_wifi_set_config(WIFI_IF_STA, &sta);
  bool up = false;
  const char *why = NULL;
  for (int i = 0; i < 3 && !up && !why; i++) {
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAILED);
    s_reason = 0;
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (b & EV_GOT_IP) up = true;
    else {
      ESP_LOGW(TAG, "连 WiFi「%s」失败，原因 %d（第 %d 次）", s_pending.ssid, s_reason, i + 1);
      why = wifi_reason_text(s_reason);
    }
  }
  if (!up) {
    esp_wifi_disconnect();
    if (!why) {
      snprintf(msg, sizeof msg, "连不上这个 WiFi（错误码 %d），请检查名称和密码，或者让板子离路由器近一点", s_reason);
      why = msg;
    }
    set_status(P_FAIL, why);
    g_state = ST_SETUP;
    say("wifi_bad");
    vTaskDelete(NULL);
  }

  // 2. 用填的 Key / 网关地址调一次接口
  bool direct = settings_direct_mode(&s_pending);
  set_status(P_CHECKING, direct ? "已连上 WiFi，正在检查 API Key…" : "已连上 WiFi，正在连接网关…");
  settings_t old = g_cfg;
  g_cfg = s_pending;
  int st = api_verify(s_mac);
  if (st == 200 && settings_save(&s_pending) == ESP_OK) {
    set_status(P_OK, "设置好啦！板子正在重启，大约 10 秒后会说「我准备好啦」。现在可以关掉这一页了。");
    g_state = ST_SETUP;
    say("saved");
    vTaskDelay(pdMS_TO_TICKS(1500));  // 让页面来得及显示成功
    esp_restart();
  }
  g_cfg = old;
  esp_wifi_disconnect();
  const char *prompt = "net_error";
  if (st == 200) set_status(P_FAIL, "保存失败，请再试一次");
  else if (st == 401 || st == 403) {
    set_status(P_FAIL, direct ? "API Key 不对：请在 aiwhy100.com/app 「设置 → API Key」重新复制一个" : "设备令牌不对：要和网关的 DEVICE_TOKENS 一致");
    prompt = "bad_key";
  } else if (st < 0) {
    set_status(P_FAIL, direct ? "连上 WiFi 了，但连不上 AI为什么100：这个 WiFi 能上网吗？" : "连上 WiFi 了，但连不上网关：检查网关地址和端口，网关要开着");
  } else {
    snprintf(msg, sizeof msg, "服务器返回 %d：请检查%s", st, direct ? "接口地址" : "网关地址");
    set_status(P_FAIL, msg);
  }
  g_state = ST_SETUP;
  say(prompt);
  vTaskDelete(NULL);
}

// ================= 页面：整页先在内存里拼好，一次发完（逐段发容易被手机的配网弹窗中途断开） =================
typedef struct { char *p; size_t n, cap; } sbuf_t;

static void sb(sbuf_t *b, const char *s) {
  size_t l = strlen(s);
  if (b->n + l + 1 > b->cap) return;
  memcpy(b->p + b->n, s, l);
  b->n += l;
  b->p[b->n] = 0;
}
/** 写 HTML 属性 / 文本（json=true 时按 JSON 字符串转义） */
static void sb_esc(sbuf_t *b, const char *s, bool json) {
  char c[2] = { 0 };
  for (; *s; s++) {
    const char *e = NULL;
    if (json) e = *s == '"' ? "\\\"" : *s == '\\' ? "\\\\" : *s == '\n' ? "\\n" : NULL;
    else e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
    if (e) sb(b, e);
    else { c[0] = *s; sb(b, c); }
  }
}
static bool sb_new(sbuf_t *b) {
  b->p = heap_caps_malloc(PAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  b->n = 0;
  b->cap = PAGE_MAX;
  if (b->p) b->p[0] = 0;
  return b->p != NULL;
}
static esp_err_t sb_send(httpd_req_t *req, sbuf_t *b, const char *type) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t e = httpd_resp_send(req, b->p, b->n);
  free(b->p);
  return e;
}

static const char *CSS =
  "<style>:root{--bg:#fff8ef;--card:#fff;--ink:#2b2118;--sub:#8a7a6a;--accent:#ff8a3d;--line:#eadccb;--ok:#2e9e5b;--bad:#d64545;--pick:#fff1e4}"
  "@media(prefers-color-scheme:dark){:root{--bg:#1c1712;--card:#29221b;--ink:#f6ece2;--sub:#b3a291;--line:#3a3027;--pick:#3a2b1e}}"
  "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 -apple-system,'PingFang SC',sans-serif}"
  "main{max-width:480px;margin:0 auto;padding:20px 16px 0}h1{font-size:22px;margin:0 0 4px}.sub{color:var(--sub);font-size:14px;margin:0 0 16px}"
  ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:14px}"
  ".step{font-weight:600;margin-bottom:10px}.step small{font-weight:400;color:var(--sub);font-size:12px;display:block}"
  ".ap{display:flex;align-items:center;gap:10px;padding:11px 12px;border:1px solid var(--line);border-radius:12px;margin-bottom:8px}"
  ".ap input{margin:0}.ap span{flex:1;word-break:break-all}.ap i{font-style:normal;color:var(--sub);font-size:12px;white-space:nowrap}"
  ".ap:has(input:checked){border-color:var(--accent);background:var(--pick)}"
  "input[type=text],input[type=password]{width:100%;padding:12px;border:1px solid var(--line);border-radius:10px;background:transparent;color:var(--ink);font:inherit;margin-top:4px}"
  ".tiny{font-size:13px;color:var(--sub);margin-top:8px;display:flex;gap:6px;align-items:center}"
  "details{margin-top:12px;font-size:14px}summary{color:var(--sub)}.mode label{display:flex;gap:8px;margin:8px 0}"
  ".bar{position:sticky;bottom:0;padding:12px 0 20px;background:linear-gradient(rgba(0,0,0,0),var(--bg) 35%)}"
  "button{width:100%;padding:15px;border:0;border-radius:999px;background:var(--accent);color:#fff;font-size:17px;font-weight:600}button:disabled{opacity:.6}"
  "a{color:var(--accent)}#st{display:none;gap:12px;align-items:flex-start}#st.on{display:flex}#st p{margin:0;font-weight:600}#st small{display:block;color:var(--sub);font-size:13px;margin-top:4px}"
  ".spin{flex:none;width:22px;height:22px;border-radius:50%;border:3px solid var(--line);border-top-color:var(--accent);animation:r 1s linear infinite;margin-top:2px}"
  "@keyframes r{to{transform:rotate(360deg)}}#st.ok .spin{animation:none;border-color:var(--ok)}#st.fail .spin{animation:none;border-color:var(--bad)}"
  "#st.ok p{color:var(--ok)}#st.fail p{color:var(--bad)}.hint{font-size:12px;color:var(--sub);text-align:center;margin:6px 0 0}"
  "</style>";

static const char *SCRIPT =
  "<script>(function(){"
  "var f=document.getElementById('f'),b=document.getElementById('b'),st=document.getElementById('st'),"
  "msg=document.getElementById('msg'),sm=document.getElementById('sm'),ssid=document.getElementById('ssid'),miss=0,timer;"
  // 选 WiFi：点列表就填好名称，并跳到密码框；选「其他」才显示手动输入
  "function pick(){var r=f.querySelector('input[name=pick]:checked');if(!r){ssid.style.display=f.querySelectorAll('input[name=pick]').length>1?'none':'';return}"
  "ssid.style.display=r.value?'none':'';if(r.value)ssid.value=r.value;else ssid.focus()}"
  "[].forEach.call(f.querySelectorAll('input[name=pick]'),function(r){r.onchange=function(){pick();if(r.value)f.pass.focus()}});pick();"
  "document.getElementById('sp').onchange=function(){f.pass.type=this.checked?'text':'password'};"
  "function mode(){var c=f.querySelector('input[name=mode]:checked'),g=!!c&&c.value=='gateway';"
  "document.getElementById('kd').style.display=g?'none':'';document.getElementById('gw').style.display=g?'':'none'}"
  "[].forEach.call(f.querySelectorAll('input[name=mode]'),function(r){r.onchange=mode});mode();"
  "function show(c,t,s){st.className='card on '+c;msg.textContent=t;sm.textContent=s||'';st.scrollIntoView({behavior:'smooth',block:'center'})}"
  "function done(t){clearInterval(timer);b.disabled=false;b.textContent=t||'保存并连接'}"
  "function poll(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){miss=0;"
  "if(s.state=='ok'){clearInterval(timer);b.textContent='设置好啦 ✓';show('ok',s.msg)}"
  "else if(s.state=='fail'){done('改好了，再试一次');show('fail',s.msg,'设置没有保存，改好后再点一次按钮')}"
  "else show('',s.msg,'板子会用声音说结果，大约 10–30 秒')}).catch(function(){"
  "if(++miss>3)show('','手机和板子的连接断了一下（板子连 WiFi 时有些手机会这样）',"
  "'听板子说话：说「设置好啦」就成功了；说「连不上」请重新连上 AI为什么100 热点，再打开 192.168.4.1')})}"
  "f.onsubmit=function(e){e.preventDefault();"
  "if(!ssid.value.trim()){show('fail','请先选你家的 WiFi');return}"
  "if(f.querySelector('input[name=mode]:checked').value!='gateway'&&!f.key.value.trim()&&!f.key.placeholder.match(/已保存/)){if(document.activeElement)document.activeElement.blur();show('fail','请填 API Key','在 aiwhy100.com/app 「设置 → API Key」创建');return}"
  "if(document.activeElement)document.activeElement.blur();"
  "b.disabled=true;b.textContent='正在连接…';show('','正在把设置发给板子…');"
  "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','Accept':'application/json'},body:new URLSearchParams(new FormData(f)).toString()})"
  ".then(function(r){return r.json()}).then(function(r){if(!r.ok){done();show('fail',r.error);return}"
  "show('','正在连接 WiFi…','板子会用声音说结果，大约 10–30 秒');miss=0;timer=setInterval(poll,1000)})"
  ".catch(function(){done();show('fail','没能把设置发给板子','请确认手机还连着 AI为什么100 热点，再点一次按钮')})};"
  "})()</script>";

static esp_err_t page_get(httpd_req_t *req) {
  ESP_LOGI(TAG, "打开设置页 %s", req->uri);
  sbuf_t b;
  if (!sb_new(&b)) return httpd_resp_send_500(req);
  bool known = false;
  sb(&b, "<!doctype html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>AI为什么100 设置</title>");
  sb(&b, CSS);
  sb(&b, "</head><body><main><h1>给板子连上网</h1><p class=sub>三步就好。填完点最下面的「保存并连接」，板子会当场试一次，并用声音告诉你结果。</p>"
         "<form id=f method=post action=/save>"
         "<div class=card><div class=step>① 选你家的 WiFi<small>只支持 2.4G；名字带 5G 的连不上</small></div>");
  for (int i = 0; i < s_naps; i++) {
    bool sel = strcmp(s_aps[i].ssid, g_cfg.ssid) == 0;
    known |= sel;
    sb(&b, "<label class=ap><input type=radio name=pick value=\"");
    sb_esc(&b, s_aps[i].ssid, false);
    sb(&b, sel ? "\" checked><span>" : "\"><span>");
    sb_esc(&b, s_aps[i].ssid, false);
    sb(&b, "</span><i>");
    sb(&b, s_aps[i].open ? "" : "🔒 ");
    sb(&b, s_aps[i].rssi > -60 ? "信号好" : s_aps[i].rssi > -75 ? "信号一般" : "信号弱");
    sb(&b, "</i></label>");
  }
  sb(&b, "<label class=ap><input type=radio name=pick value=\"\"");
  sb(&b, known || !s_naps ? (s_naps ? "" : " checked") : (g_cfg.ssid[0] ? " checked" : ""));
  sb(&b, "><span>其他（手动输入名称）</span></label><input type=text id=ssid name=ssid autocomplete=off autocapitalize=off enterkeyhint=next placeholder='WiFi 名称' value=\"");
  sb_esc(&b, g_cfg.ssid, false);
  sb(&b, "\"></div><div class=card><div class=step>② WiFi 密码</div><input type=password name=pass autocomplete=off autocapitalize=off enterkeyhint=next placeholder=\"");
  sb(&b, g_cfg.pass[0] ? "已保存；WiFi 没换就留空" : "没有密码就留空");
  sb(&b, "\"><label class=tiny><input type=checkbox id=sp>显示密码</label></div>"
         "<div class=card><div class=step>③ AI为什么100 的 API Key<small>在手机上打开 aiwhy100.com/app，「设置 → API Key」新建一个，复制过来</small></div>"
         "<div id=kd><input type=password name=key autocomplete=off autocapitalize=off enterkeyhint=go placeholder=\"");
  sb(&b, g_cfg.key[0] ? "已保存；不换就留空" : "svf_live_…");
  sb(&b, "\"></div><details");
  sb(&b, settings_direct() ? "" : " open");
  sb(&b, "><summary>高级：连自建网关（厂商用）</summary><div class=mode><label><input type=radio name=mode value=direct");
  sb(&b, settings_direct() ? " checked" : "");
  sb(&b, ">直连 AI为什么100（默认）</label><label><input type=radio name=mode value=gateway");
  sb(&b, settings_direct() ? "" : " checked");
  sb(&b, ">自建网关</label></div><div id=gw><input type=text name=server autocomplete=off autocapitalize=off placeholder='网关地址，如 http://192.168.1.10:8080' value=\"");
  if (!settings_direct()) sb_esc(&b, g_cfg.server, false);
  sb(&b, "\"><input type=password name=token autocomplete=off placeholder=\"");
  sb(&b, g_cfg.token[0] ? "设备令牌：已保存；不换就留空" : "设备令牌（网关设了 DEVICE_TOKENS 才填）");
  sb(&b, "\"></div></details></div>"
         "<div id=st class=card><div class=spin></div><div><p id=msg></p><small id=sm></small></div></div>"
         "<div class=bar><button id=b type=submit>保存并连接</button><p class=hint>录音只用来识别成文字，识别完就丢，不保存</p></div>"
         "</form></main>");
  sb(&b, SCRIPT);
  sb(&b, "</body></html>");
  if (b.n + 64 > b.cap) ESP_LOGW(TAG, "设置页太长被截断（%u 字节）", (unsigned)b.n);
  return sb_send(req, &b, "text/html; charset=utf-8");
}

/** 不支持脚本时的结果页：每 2 秒自己刷新 */
static esp_err_t wait_page(httpd_req_t *req) {
  sbuf_t b;
  if (!sb_new(&b)) return httpd_resp_send_500(req);
  bool end = s_pstate == P_OK || s_pstate == P_FAIL;
  sb(&b, "<!doctype html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  if (!end) sb(&b, "<meta http-equiv=refresh content='2;url=/wait'>");
  sb(&b, "<title>AI为什么100 设置</title>");
  sb(&b, CSS);
  sb(&b, "</head><body><main><h1>给板子连上网</h1><div id=st class='card on ");
  sb(&b, s_pstate == P_OK ? "ok" : s_pstate == P_FAIL ? "fail" : "");
  sb(&b, "'><div class=spin></div><div><p>");
  sb_esc(&b, s_pmsg[0] ? s_pmsg : "正在连接 WiFi…", false);
  sb(&b, "</p><small>");
  sb(&b, s_pstate == P_FAIL ? "<a href=/>‹ 返回修改</a>" : end ? "" : "板子会用声音说结果，大约 10–30 秒");
  sb(&b, "</small></div></div></main></body></html>");
  return sb_send(req, &b, "text/html; charset=utf-8");
}

static esp_err_t wait_get(httpd_req_t *req) { return wait_page(req); }

static esp_err_t save_reply(httpd_req_t *req, bool json, const char *error) {
  if (!json) {
    if (!error) {
      httpd_resp_set_status(req, "303 See Other");
      httpd_resp_set_hdr(req, "Location", "/wait");
      return httpd_resp_send(req, NULL, 0);
    }
    set_status(P_FAIL, error);
    return wait_page(req);
  }
  sbuf_t b;
  if (!sb_new(&b)) return httpd_resp_send_500(req);
  if (!error) sb(&b, "{\"ok\":true}");
  else {
    sb(&b, "{\"ok\":false,\"error\":\"");
    sb_esc(&b, error, true);
    sb(&b, "\"}");
  }
  return sb_send(req, &b, "application/json; charset=utf-8");
}

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
static void field(const char *body, const char *key, char *out, size_t max) {
  out[0] = 0;
  if (httpd_query_key_value(body, key, out, max) == ESP_OK) url_decode(out);
}
static void trim(char *s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  size_t k = 0;
  while (s[k] && isspace((unsigned char)s[k])) k++;
  if (k) memmove(s, s + k, n - k + 1);
}

static esp_err_t save_post(httpd_req_t *req) {
  char accept[64] = "";
  httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof accept);
  bool json = strstr(accept, "application/json") != NULL;
  ESP_LOGI(TAG, "收到设置（%d 字节，%s）", req->content_len, json ? "页面脚本" : "表单");
  if (busy()) return save_reply(req, json, "上一次的设置还在验证，请稍等");

  static char body[1024];
  int len = req->content_len < (int)sizeof body - 1 ? req->content_len : (int)sizeof body - 1, got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, body + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = 0;

  settings_t s = g_cfg;
  char buf[160];
  field(body, "pick", buf, sizeof buf);               // 列表里选的；选「其他」时为空，用手动输入的名称
  if (!buf[0]) field(body, "ssid", buf, sizeof buf);
  trim(buf);
  strlcpy(s.ssid, buf, sizeof s.ssid);
  field(body, "pass", buf, sizeof buf);
  // WiFi 名称没变、密码留空：沿用原来的密码
  if (buf[0] || strcmp(s.ssid, g_cfg.ssid) != 0) strlcpy(s.pass, buf, sizeof s.pass);
  field(body, "mode", buf, sizeof buf);
  strlcpy(s.mode, strcmp(buf, "gateway") == 0 ? "gateway" : "direct", sizeof s.mode);
  field(body, "key", buf, sizeof buf);
  trim(buf);
  if (buf[0]) strlcpy(s.key, buf, sizeof s.key);
  field(body, "server", buf, sizeof buf);
  trim(buf);
  strlcpy(s.server, strcmp(s.mode, "gateway") == 0 ? buf : "", sizeof s.server);
  for (size_t n = strlen(s.server); n && s.server[n - 1] == '/'; n--) s.server[n - 1] = 0;
  field(body, "token", buf, sizeof buf);
  trim(buf);
  if (buf[0]) strlcpy(s.token, buf, sizeof s.token);

  const char *err = !s.ssid[0] ? "请先选你家的 WiFi"
                  : strcmp(s.mode, "direct") == 0 && !s.key[0] ? "请填 API Key（在 aiwhy100.com/app 「设置 → API Key」创建）"
                  : strcmp(s.mode, "gateway") == 0 && strncmp(s.server, "http", 4) != 0 ? "网关地址要以 http:// 或 https:// 开头"
                  : NULL;
  if (err) {
    ESP_LOGW(TAG, "设置不完整：%s", err);
    return save_reply(req, json, err);
  }
  ESP_LOGI(TAG, "开始验证：WiFi「%s」，方式 %s", s.ssid, s.mode);
  s_pending = s;
  set_status(P_CONNECTING, "正在连接 WiFi…");
  save_reply(req, json, NULL);
  xTaskCreate(verify_task, "verify", 8192, NULL, 5, NULL);
  return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req) {
  sbuf_t b;
  if (!sb_new(&b)) return httpd_resp_send_500(req);
  sb(&b, "{\"state\":\"");
  sb(&b, PSTATE[s_pstate]);
  sb(&b, "\",\"msg\":\"");
  sb_esc(&b, s_pmsg, true);
  sb(&b, "\"}");
  return sb_send(req, &b, "application/json; charset=utf-8");
}

/** 手机的联网检测（/generate_204、/hotspot-detect.html……）全部跳到设置页，触发「登录 WLAN」弹窗 */
static esp_err_t redirect_get(httpd_req_t *req) {
  ESP_LOGI(TAG, "跳转 %s", req->uri);
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
  esp_wifi_scan_get_ap_records(&n, recs);  // 按信号强弱排好了
  s_naps = 0;
  for (int i = 0; i < n; i++) {
    const char *ssid = (const char *)recs[i].ssid;
    if (!ssid[0]) continue;
    bool dup = false;
    for (int k = 0; k < s_naps; k++) dup |= strcmp(s_aps[k].ssid, ssid) == 0;
    if (dup) continue;
    strlcpy(s_aps[s_naps].ssid, ssid, sizeof s_aps[0].ssid);
    s_aps[s_naps].rssi = recs[i].rssi;
    s_aps[s_naps].open = recs[i].authmode == WIFI_AUTH_OPEN;
    s_naps++;
  }
  ESP_LOGI(TAG, "附近 %d 个 WiFi", s_naps);
}

void portal_run(void) {
  g_portal = true;
  g_state = ST_SETUP;
  s_ev = xEventGroupCreate();
  esp_wifi_stop();
  esp_netif_create_default_wifi_ap();
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
  int n = snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "AI为什么100-%02X%02X", mac[4], mac[5]);
  ap.ap.ssid_len = n;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  // STA 先不连任何网络，只用来扫描附近的 WiFi；保存时再用它试连
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
  cfg.max_uri_handlers = 8;
  httpd_handle_t srv = NULL;
  ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
  const httpd_uri_t uris[] = {
    { .uri = "/", .method = HTTP_GET, .handler = page_get },
    { .uri = "/save", .method = HTTP_POST, .handler = save_post },
    { .uri = "/status", .method = HTTP_GET, .handler = status_get },
    { .uri = "/wait", .method = HTTP_GET, .handler = wait_get },
    { .uri = "/*", .method = HTTP_GET, .handler = redirect_get },
  };
  for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);

  say("setup");
  int64_t t0 = esp_timer_get_time() / 1000, last_say = t0;
  int reminds = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(300));
    int64_t now = esp_timer_get_time() / 1000;
    // 手机连上热点：告诉家长下一步做什么（验证中不打断）
    if (s_phone_joined && !busy()) {
      s_phone_joined = false;
      if (s_pstate != P_OK) {
        say("phone_in");
        last_say = now;
      }
    }
    // 没人连热点：每 2 分钟提醒一次，最多 5 次
    if (s_phones == 0 && s_pstate == P_IDLE && now - last_say > REMIND_MS && reminds < MAX_REMINDS) {
      reminds++;
      say("setup");
      last_say = now;
    }
    // 验证中不超时；15 分钟没保存成功就重启
    if (!busy() && now - t0 > PORTAL_TIMEOUT_MS) {
      ESP_LOGW(TAG, "15 分钟没有保存，重启");
      esp_restart();
    }
  }
}
