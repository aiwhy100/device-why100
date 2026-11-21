// 接口：两种接入方式
//   直连（direct）：设备直接调 AI为什么100 个人开发者接口（docs/api.md）
//     录音 → POST /api/app/v1/asr → 问句 → POST /questions/match（小奇讲过啦，不扣额度）→ 没有就 POST /videos
//     → 记下视频 ID（存 NVS，重启不丢）→ 每 30 秒 GET /videos/<id>，做好了播报
//   网关（gateway）：设备只和自建网关通信（docs/gateway.md「设备接口」），Key 在网关那边
//     POST /api/device/<MAC>/ask → 网关返回要播的提示语；GET /poll → 有没有做好的；POST /ack 确认
// 录音只在内存里，上传完就丢。
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "app.h"

static const char *TAG = "api";
static char s_device[18];

#define RESP_MAX     8192
#define MAX_PENDING  8
#define ID_MAX       40
static char s_pending[MAX_PENDING][ID_MAX];
static int s_npending;

// ================= HTTP =================
/** 发请求，响应正文写进 resp（以 0 结尾）。返回 HTTP 状态码，网络失败 < 0 */
static int http(const char *method, const char *url, const char *ctype, const void *body, int len, char *resp, int max) {
  esp_http_client_config_t cfg = {
    .url = url,
    .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    .timeout_ms = 20000,
    .buffer_size = 2048,
    .buffer_size_tx = 1024,
    .crt_bundle_attach = strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -1;
  if (settings_direct()) {
    char auth[128];
    snprintf(auth, sizeof auth, "Bearer %s", g_cfg.key);
    esp_http_client_set_header(c, "Authorization", auth);
  } else if (g_cfg.token[0]) {
    esp_http_client_set_header(c, "X-Device-Token", g_cfg.token);
  }
  if (body) esp_http_client_set_header(c, "Content-Type", ctype);
  int status = -1;
  resp[0] = 0;
  if (esp_http_client_open(c, body ? len : 0) == ESP_OK) {
    int w = 0;
    while (body && w < len) {
      int k = esp_http_client_write(c, (const char *)body + w, len - w);
      if (k <= 0) break;
      w += k;
    }
    if (!body || w == len) {
      esp_http_client_fetch_headers(c);
      status = esp_http_client_get_status_code(c);
      int r = esp_http_client_read_response(c, resp, max - 1);
      resp[r > 0 ? r : 0] = 0;
    }
  }
  esp_http_client_cleanup(c);
  return status;
}

static char *resp_buf(void) {
  static char *buf;
  if (!buf) buf = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return buf;
}

static const char *json_str(cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItem(o, k);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/** 失败状态码 → 提示语 */
static const char *prompt_for(int st) {
  switch (st) {
    case 400: case 422: return "rejected";
    case 401: case 403: return "bad_key";
    case 402: return "no_credit";
    case 429: return "busy";
    default: return "net_error";
  }
}

// ================= 在做的视频（直连）：存 NVS，重启后接着查 =================
static void pending_save(void) {
  char s[MAX_PENDING * ID_MAX] = "";
  for (int i = 0; i < s_npending; i++) {
    if (i) strlcat(s, ",", sizeof s);
    strlcat(s, s_pending[i], sizeof s);
  }
  nvs_handle_t h;
  if (nvs_open("why100", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "pending", s);
    nvs_commit(h);
    nvs_close(h);
  }
}

static void pending_load(void) {
  char s[MAX_PENDING * ID_MAX] = "";
  size_t len = sizeof s;
  nvs_handle_t h;
  if (nvs_open("why100", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_str(h, "pending", s, &len);
    nvs_close(h);
  }
  s_npending = 0;
  for (char *save = NULL, *t = strtok_r(s, ",", &save); t && s_npending < MAX_PENDING; t = strtok_r(NULL, ",", &save))
    strlcpy(s_pending[s_npending++], t, ID_MAX);
  if (s_npending) ESP_LOGI(TAG, "接着查 %d 个在做的视频", s_npending);
}

static void pending_add(const char *id) {
  if (s_npending == MAX_PENDING) memmove(s_pending[0], s_pending[1], (MAX_PENDING - 1) * ID_MAX), s_npending--;
  strlcpy(s_pending[s_npending++], id, ID_MAX);
  pending_save();
}

static void pending_remove(int i) {
  memmove(s_pending[i], s_pending[i + 1], (s_npending - i - 1) * ID_MAX);
  s_npending--;
  pending_save();
}

int api_pending(void) { return settings_direct() ? s_npending : 0; }

void api_init(const char *device_id) {
  strlcpy(s_device, device_id, sizeof s_device);
  if (settings_direct()) pending_load();
}

// ================= 直连 =================
static const char *direct_ask(const int16_t *pcm, size_t samples) {
  char *resp = resp_buf();
  char url[256];
  const char *base = settings_server();

  // 1. 识别
  snprintf(url, sizeof url, "%s/api/app/v1/asr?format=pcm16", base);
  int st = http("POST", url, "application/octet-stream", pcm, samples * 2, resp, RESP_MAX);
  ESP_LOGI(TAG, "识别 %.1f 秒 → %d %s", samples / 16000.f, st, resp);
  if (st != 200) return st == 429 ? "busy" : prompt_for(st);
  cJSON *j = cJSON_Parse(resp);
  char question[256] = "";
  const char *q = json_str(j, "question");
  if (q) strlcpy(question, q, sizeof question);
  cJSON_Delete(j);
  if (strlen(question) < 12) return "not_heard";  // 不到三个汉字（UTF-8 一个汉字 3 字节，含问号）

  // 2. 小奇讲过啦？（不扣额度）
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "question", question);
  char *b = cJSON_PrintUnformatted(body);
  snprintf(url, sizeof url, "%s/api/app/v1/questions/match", base);
  st = http("POST", url, "application/json", b, strlen(b), resp, RESP_MAX);
  if (st == 200) {
    j = cJSON_Parse(resp);
    cJSON *m = cJSON_GetObjectItem(j, "match");
    if (cJSON_IsObject(m)) {
      const char *id = json_str(cJSON_GetObjectItem(m, "video"), "id");
      bool own = cJSON_IsTrue(cJSON_GetObjectItem(m, "own"));
      int st2 = 200;
      if (id && !own) {
        snprintf(url, sizeof url, "%s/api/app/v1/videos/%s/save", base, id);
        char r2[64];
        st2 = http("POST", url, "application/json", "{}", 2, r2, sizeof r2);
      }
      cJSON_Delete(j);
      if (id && st2 == 200) {
        ESP_LOGI(TAG, "小奇讲过啦「%s」→ %s", question, id);
        cJSON_free(b);
        cJSON_Delete(body);
        return "found";
      }
    } else {
      cJSON_Delete(j);
    }
  }

  // 3. 提交
  cJSON_AddStringToObject(body, "channel", "kids");
  cJSON_free(b);
  b = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  snprintf(url, sizeof url, "%s/api/app/v1/videos", base);
  st = http("POST", url, "application/json", b, strlen(b), resp, RESP_MAX);
  cJSON_free(b);
  ESP_LOGI(TAG, "提交「%s」→ %d", question, st);
  if (st != 200 && st != 201) {
    ESP_LOGW(TAG, "%s", resp);
    return prompt_for(st);
  }
  j = cJSON_Parse(resp);
  const char *id = json_str(j, "id");
  if (id) pending_add(id);
  cJSON_Delete(j);
  return id ? "got" : "net_error";
}

static const char *direct_check(void) {
  char *resp = resp_buf();
  char url[256];
  const char *say_what = NULL;
  for (int i = 0; i < s_npending;) {
    snprintf(url, sizeof url, "%s/api/app/v1/videos/%s", settings_server(), s_pending[i]);
    int st = http("GET", url, NULL, NULL, 0, resp, RESP_MAX);
    if (st == 404) { pending_remove(i); continue; }
    if (st != 200) return say_what;  // 网络不好，下次再查
    cJSON *j = cJSON_Parse(resp);
    const char *status = json_str(j, "status");
    bool done = status && strcmp(status, "done") == 0;
    bool failed = status && (strcmp(status, "failed") == 0 || strcmp(status, "cancelled") == 0);
    if (done || failed) ESP_LOGI(TAG, "视频 %s %s", s_pending[i], status);
    cJSON_Delete(j);
    if (done) { say_what = "ready"; pending_remove(i); continue; }
    if (failed) { if (!say_what) say_what = "failed"; pending_remove(i); continue; }
    i++;
  }
  return say_what;
}

// ================= 网关 =================
static int gw(const char *action, const char *ctype, const void *body, int len, char *resp, int max) {
  char url[256];
  snprintf(url, sizeof url, "%s/api/device/%s/%s", settings_server(), s_device, action);
  return http(body ? "POST" : "GET", url, ctype, body, len, resp, max);
}

static const char *gateway_ask(const int16_t *pcm, size_t samples) {
  char *resp = resp_buf();
  int st = gw("ask", "application/octet-stream", pcm, samples * 2, resp, RESP_MAX);
  ESP_LOGI(TAG, "ask %.1f 秒 → %d %s", samples / 16000.f, st, resp);
  if (st == 401) return "bad_key";
  static char prompt[16];
  strlcpy(prompt, "net_error", sizeof prompt);
  cJSON *j = st == 200 ? cJSON_Parse(resp) : NULL;
  const char *p = json_str(j, "prompt");
  if (p) strlcpy(prompt, p, sizeof prompt);
  cJSON_Delete(j);
  return prompt;
}

static const char *gateway_check(void) {
  char *resp = resp_buf();
  if (gw("poll", NULL, NULL, 0, resp, RESP_MAX) != 200) return NULL;
  cJSON *j = cJSON_Parse(resp);
  cJSON *ids = cJSON_GetObjectItem(j, "ids");
  cJSON *fids = cJSON_GetObjectItem(j, "failedIds");
  bool ready = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) > 0;
  bool failed = cJSON_IsArray(fids) && cJSON_GetArraySize(fids) > 0;
  if (ready || failed) {
    // 一起确认掉，免得重复播报
    cJSON *ack = cJSON_CreateObject();
    cJSON *all = cJSON_AddArrayToObject(ack, "ids");
    cJSON *it;
    if (cJSON_IsArray(ids)) cJSON_ArrayForEach(it, ids) cJSON_AddItemToArray(all, cJSON_Duplicate(it, 0));
    if (cJSON_IsArray(fids)) cJSON_ArrayForEach(it, fids) cJSON_AddItemToArray(all, cJSON_Duplicate(it, 0));
    char *body = cJSON_PrintUnformatted(ack);
    char r2[64];
    gw("ack", "application/json", body, strlen(body), r2, sizeof r2);
    cJSON_free(body);
    cJSON_Delete(ack);
  }
  cJSON_Delete(j);
  return ready ? "ready" : failed ? "failed" : NULL;
}

// ================= 对外 =================
const char *api_ask(const int16_t *pcm, size_t samples) {
  return settings_direct() ? direct_ask(pcm, samples) : gateway_ask(pcm, samples);
}

const char *api_check(void) {
  return settings_direct() ? direct_check() : gateway_check();
}
