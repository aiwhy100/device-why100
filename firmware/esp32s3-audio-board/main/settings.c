// 设置：NVS（命名空间 why100）里有就用 NVS，没有就用 menuconfig 里的默认值
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "app.h"

static const char *TAG = "settings";
static const char *NS = "why100";
settings_t g_cfg;

static void load_str(nvs_handle_t h, const char *k, char *out, size_t max, const char *def) {
  size_t len = max;
  if (!h || nvs_get_str(h, k, out, &len) != ESP_OK) strlcpy(out, def, max);
}

void settings_load(void) {
  nvs_handle_t h = 0;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) h = 0;
  load_str(h, "ssid", g_cfg.ssid, sizeof g_cfg.ssid, CONFIG_WHY_WIFI_SSID);
  load_str(h, "pass", g_cfg.pass, sizeof g_cfg.pass, CONFIG_WHY_WIFI_PASS);
#if CONFIG_WHY_MODE_GATEWAY
  load_str(h, "mode", g_cfg.mode, sizeof g_cfg.mode, "gateway");
#else
  load_str(h, "mode", g_cfg.mode, sizeof g_cfg.mode, "direct");
#endif
  load_str(h, "key", g_cfg.key, sizeof g_cfg.key, CONFIG_WHY_API_KEY);
  load_str(h, "server", g_cfg.server, sizeof g_cfg.server, CONFIG_WHY_SERVER_URL);
  load_str(h, "token", g_cfg.token, sizeof g_cfg.token, CONFIG_WHY_DEVICE_TOKEN);
  if (h) nvs_close(h);
  // 去掉地址末尾的斜杠
  for (size_t n = strlen(g_cfg.server); n && g_cfg.server[n - 1] == '/'; n--) g_cfg.server[n - 1] = 0;
  ESP_LOGI(TAG, "WiFi「%s」，方式 %s，服务器 %s%s", g_cfg.ssid, g_cfg.mode, settings_server(),
           settings_direct() ? (g_cfg.key[0] ? "，Key 已设置" : "，Key 未设置") : "");
}

esp_err_t settings_save(const settings_t *s) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  if ((e = nvs_set_str(h, "ssid", s->ssid)) == ESP_OK && (e = nvs_set_str(h, "pass", s->pass)) == ESP_OK &&
      (e = nvs_set_str(h, "mode", s->mode)) == ESP_OK && (e = nvs_set_str(h, "key", s->key)) == ESP_OK &&
      (e = nvs_set_str(h, "server", s->server)) == ESP_OK && (e = nvs_set_str(h, "token", s->token)) == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}

bool settings_direct_mode(const settings_t *s) { return strcmp(s->mode, "gateway") != 0; }
bool settings_direct(void) { return settings_direct_mode(&g_cfg); }

const char *settings_server(void) {
  if (g_cfg.server[0]) return g_cfg.server;
  return settings_direct() ? "https://api.aiwhy100.com" : "";
}

bool settings_ready(void) {
  if (!g_cfg.ssid[0]) return false;
  return settings_direct() ? g_cfg.key[0] != 0 : g_cfg.server[0] != 0;
}
