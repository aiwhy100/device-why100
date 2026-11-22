// 各模块共用的声明
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// ================= 设置（settings.c） =================
// 保存在 NVS 里；第一次开机或按键进入配网时，用手机网页填写（portal.c）。
// menuconfig「AI为什么100」里的值是出厂默认值，NVS 里有值时以 NVS 为准。
typedef struct {
  char ssid[33];
  char pass[65];
  char mode[8];      // "direct"：直连 AI为什么100；"gateway"：连自建网关
  char key[96];      // 直连：个人 API Key（svf_live_…）
  char server[128];  // 直连：接口地址，留空 = https://api.aiwhy100.com；网关：http://<IP>:8080
  char token[65];    // 网关：设备令牌（网关设了 DEVICE_TOKENS 时）
} settings_t;

extern settings_t g_cfg;
void settings_load(void);
esp_err_t settings_save(const settings_t *s);
/** WiFi 和接入方式都填好了 */
bool settings_ready(void);
bool settings_direct(void);
bool settings_direct_mode(const settings_t *s);
/** 去掉末尾斜杠的接口地址 */
const char *settings_server(void);

// ================= 配网（portal.c） =================
/** 开热点「AI为什么100-xxxx」+ 设置网页，保存后重启；15 分钟没保存也重启。不返回 */
void portal_run(void) __attribute__((noreturn));
/** 正在配网（main.c 的 WiFi 事件处理据此不自动重连） */
extern volatile bool g_portal;

// ================= 接口（api.c） =================
void api_init(const char *device_id);
/** 录音（16kHz 单声道 16 位）→ 识别 → 提交，返回要播的提示语 id（got / found / not_heard / rejected / …） */
const char *api_ask(const int16_t *pcm, size_t samples);
/** 有没有做好 / 没做成的视频：返回 "ready"、"failed" 或 NULL */
const char *api_check(void);
/** 还有几个视频在做 */
int api_pending(void);
/** 用当前设置试一下能不能用：直连 GET /me，网关 GET /poll。返回 HTTP 状态码，网络失败 < 0 */
int api_verify(const char *device_id);

// ================= 状态灯 / 提示语（main.c） =================
typedef enum { ST_BOOT, ST_IDLE, ST_LISTEN, ST_THINK, ST_SPEAK, ST_READY, ST_OFFLINE, ST_SETUP } state_t;
extern volatile state_t g_state;
void say(const char *id);
