/*
  AI为什么100 · Arduino 示例（ESP32-S3 / ESP32，arduino-esp32 3.x）

  最少只要一块 ESP32 开发板：在「串口监视器」里打一个「为什么」问题，几分钟后串口打印视频链接。
  再接一个 INMP441 麦克风（约 10 元），就能按住 BOOT 键说话提问。

  接线（INMP441 → ESP32-S3，引脚可以在下面改）：
    VDD → 3V3    GND → GND    L/R → GND（左声道）
    SCK → GPIO 4   WS → GPIO 5   SD → GPIO 6

  用法：
    1. 在 https://aiwhy100.com/app/ 注册，「设置 → API Key」创建一个 Key
    2. 填下面的 WIFI_SSID / WIFI_PASS / API_KEY，选好开发板，上传
    3. 打开串口监视器（115200，换行符选「换行」），输入「为什么天空是蓝色的？」回车；
       或者按住 BOOT 键说话，松开提交
    4. 做好后串口打印播放链接；手机打开 https://aiwhy100.com/app/ 登录同一个账号也能看

  本地联调不花额度：电脑上运行 node mock/server.mjs，把 API_BASE 改成 http://<电脑 IP>:8787

  流程（接口见 docs/api.md）：
    录音 → POST /api/app/v1/asr?format=pcm16 → 问句
    → POST /questions/match（「小奇讲过啦」：已有成片直接收进账号，不扣额度）
    → 没有就 POST /videos → 每 30 秒 GET /videos/<id>，status=done 后打印链接
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ESP_I2S.h>

// ======== 改这里 ========
const char *WIFI_SSID = "your-2.4G-wifi";
const char *WIFI_PASS = "password";
const char *API_KEY   = "svf_live_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
const char *API_BASE  = "https://api.aiwhy100.com";   // 本地模拟服务器：http://192.168.1.10:8787

#define USE_MIC      1     // 没接麦克风就改成 0，只用串口打字提问
#define PIN_I2S_SCK  4
#define PIN_I2S_WS   5
#define PIN_I2S_SD   6
#define PIN_BUTTON   0     // BOOT 键，按下为低
#define MAX_REC_SEC  8
// =======================

#define SAMPLE_RATE 16000
I2SClass i2s;
int16_t *rec = nullptr;
size_t recMax = 0;

String pending[4];   // 在做的视频 ID
int npending = 0;
unsigned long lastPoll = 0;

// ---------- 状态灯（开发板自带的 RGB 灯；没有就只看串口） ----------
void led(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, r, g, b);
#endif
}

// ---------- HTTP ----------
/** 发请求，返回状态码（网络失败 < 0），响应正文写进 out */
int api(const char *method, const String &path, const uint8_t *body, size_t len, const char *ctype, String &out) {
  String url = String(API_BASE) + "/api/app/v1" + path;
  NetworkClientSecure tls;
  NetworkClient plain;
  HTTPClient http;
  bool https = url.startsWith("https://");
  if (https) {
    // 用芯片里自带的根证书包校验服务器证书（不要用 setInsecure()，否则 Key 可能被中间人截获）
    tls.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
  }
  if (!(https ? http.begin(tls, url) : http.begin(plain, url))) return -1;
  http.setTimeout(20000);
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
  int st;
  if (strcmp(method, "GET") == 0) {
    st = http.GET();
  } else {
    http.addHeader("Content-Type", ctype);
    st = http.POST((uint8_t *)body, len);
  }
  out = st > 0 ? http.getString() : String();
  http.end();
  return st;
}
int apiJson(const char *method, const String &path, const String &json, String &out) {
  return api(method, path, (const uint8_t *)json.c_str(), json.length(), "application/json", out);
}

/** 从 JSON 里取 "key":"值"（够用的简易解析：只处理 \" 和 \\ 转义） */
String jsonStr(const String &s, const char *key, int from = 0) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k, from);
  if (i < 0) return "";
  String v;
  for (i += k.length(); i < (int)s.length() && s[i] != '"'; i++) {
    if (s[i] == '\\' && i + 1 < (int)s.length()) i++;
    v += s[i];
  }
  return v;
}

String escapeJson(const String &s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

/** 提错时给出提示 */
void explain(int st, const String &resp) {
  String msg = jsonStr(resp, "message");
  Serial.printf("⚠️  %d %s\n", st, msg.c_str());
  if (st == 401) Serial.println("   API Key 不对：检查 API_KEY");
  if (st == 402) Serial.println("   额度用完了：在 aiwhy100.com/app 里看看");
  if (st == 422) Serial.println("   这个问题不适合做成视频，换一个吧");
  if (st == 429) Serial.println("   太频繁了：同时最多 2 个在做、每小时最多 5 个");
  if (st < 0) Serial.println("   连不上服务器：检查网络和 API_BASE");
}

// ---------- 提问 ----------
void ask(String question) {
  question.trim();
  if (question.length() < 9) {  // 不到三个汉字
    Serial.println("🤔 没听清，再说一遍吧");
    return;
  }
  if (!question.endsWith("？") && !question.endsWith("?")) question += "？";
  Serial.printf("❓ %s\n", question.c_str());
  led(40, 0, 60);
  String resp, body = "{\"question\":\"" + escapeJson(question) + "\"}";

  // 1. 小奇讲过啦？（不扣额度）
  if (apiJson("POST", "/questions/match", body, resp) == 200 && resp.indexOf("\"match\":null") < 0) {
    int v = resp.indexOf("\"video\"");
    String id = jsonStr(resp, "id", v < 0 ? 0 : v);
    bool own = resp.indexOf("\"own\":true") >= 0;
    String r2;
    if (id.length() && (own || apiJson("POST", "/videos/" + id + "/save", "{}", r2) == 200)) {
      Serial.println("✨ 这个问题小奇讲过啦，不扣额度。手机打开 https://aiwhy100.com/app/ 就能看");
      led(0, 40, 0);
      return;
    }
  }

  // 2. 提交制作（kids：给 5–12 岁孩子讲，约 100 秒）
  body = "{\"question\":\"" + escapeJson(question) + "\",\"channel\":\"kids\"}";
  int st = apiJson("POST", "/videos", body, resp);
  if (st != 200 && st != 201) {
    explain(st, resp);
    led(60, 0, 0);
    return;
  }
  String id = jsonStr(resp, "id");
  Serial.printf("📮 已提交，%s，视频 ID %s，大约 10 分钟做好\n", jsonStr(resp, "statusText").c_str(), id.c_str());
  if (npending < 4) pending[npending++] = id;
  lastPoll = millis();
  led(0, 0, 0);
}

/** 查在做的视频 */
void poll() {
  for (int i = 0; i < npending;) {
    String resp;
    int st = api("GET", "/videos/" + pending[i], nullptr, 0, nullptr, resp);
    if (st != 200 && st != 404) return;  // 网络不好，下次再查
    String status = jsonStr(resp, "status");
    if (st == 404 || status == "done" || status == "failed" || status == "cancelled") {
      if (status == "done") {
        Serial.printf("🎬 做好啦：「%s」\n   播放（1 小时内有效）：%s\n   分享（24 小时内有效）：%s\n",
                      jsonStr(resp, "title").c_str(), jsonStr(resp, "video").c_str(), jsonStr(resp, "share").c_str());
        led(0, 60, 20);
      } else if (st == 200) {
        Serial.printf("❌ %s：%s\n", jsonStr(resp, "statusText").c_str(), jsonStr(resp, "failReason").c_str());
      }
      for (int k = i; k < npending - 1; k++) pending[k] = pending[k + 1];
      npending--;
      continue;
    }
    Serial.printf("⏳ %s\n", jsonStr(resp, "statusText").c_str());
    i++;
  }
}

// ---------- 录音（INMP441：24 位数据放在 32 位槽里） ----------
#if USE_MIC
size_t record() {
  size_t n = 0;
  int32_t chunk[320];
  led(0, 0, 60);
  while (n + 320 <= recMax) {
    size_t got = i2s.readBytes((char *)chunk, sizeof chunk) / 4;
    for (size_t k = 0; k < got && n < recMax; k++) {
      int32_t s = chunk[k] >> 14;  // 取高位，约 +12dB 增益
      rec[n++] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    }
    if (digitalRead(PIN_BUTTON) == HIGH) break;
  }
  led(0, 0, 0);
  return n;
}

void askByVoice() {
  size_t n = record();
  if (n < SAMPLE_RATE / 2) return;  // 不到半秒：误触
  Serial.printf("🎙  录了 %.1f 秒，识别中……\n", n / (float)SAMPLE_RATE);
  String resp;
  int st = api("POST", "/asr?format=pcm16", (const uint8_t *)rec, n * 2, "application/octet-stream", resp);
  memset(rec, 0, n * 2);  // 录音用完就清掉
  if (st != 200) return explain(st, resp);
  ask(jsonStr(resp, "question"));
}
#endif

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  led(10, 10, 10);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("连接 WiFi「%s」", WIFI_SSID);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print('.'); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " 已连接" : " 连不上（只支持 2.4G），稍后自动重试");

#if USE_MIC
  i2s.setPins(PIN_I2S_SCK, PIN_I2S_WS, -1, PIN_I2S_SD);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("麦克风初始化失败：检查接线，或把 USE_MIC 改成 0");
  }
  // 有 PSRAM 录 8 秒；没有就录 4 秒（128 KB）
  recMax = SAMPLE_RATE * (psramFound() ? MAX_REC_SEC : 4);
  rec = (int16_t *)(psramFound() ? ps_malloc(recMax * 2) : malloc(recMax * 2));
  Serial.println("按住 BOOT 键说话，或者在串口里打字提问");
#else
  Serial.println("在串口里打一个「为什么」问题，回车提交");
#endif
  led(0, 0, 0);
}

void loop() {
  if (Serial.available()) ask(Serial.readStringUntil('\n'));
#if USE_MIC
  if (rec && digitalRead(PIN_BUTTON) == LOW) askByVoice();
#endif
  if (npending && millis() - lastPoll > 30000) {
    lastPoll = millis();
    poll();
  }
  delay(10);
}
