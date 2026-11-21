// 「AI为什么100」设备固件（微雪 ESP32-S3-AUDIO-Board）
// 按住任意键说话 → 松开上传录音 → 播提示语 → 定时查有没有做好的视频，做好了播报「做好啦」。
// 灯环表示状态：待机呼吸（暖橙）/ 听（蓝，随音量变亮）/ 思考（紫色转圈）/ 说话（绿）/ 做好了（彩虹）/ 没网（红闪）/ 配网（青色呼吸）
// 第一次开机（或开机时按住 K1/K2/K3 两秒、没网时按一下按键）进入手机配网，见 portal.c。
// 录音只在内存里，上传完就清掉。
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "led_strip.h"
#include "app.h"

static const char *TAG = "why";

// ---------- 引脚（微雪原理图 v1.1）；换别的板子主要改这里和 audio_init() ----------
#define PIN_I2C_SCL   GPIO_NUM_10
#define PIN_I2C_SDA   GPIO_NUM_11
#define PIN_I2S_MCLK  GPIO_NUM_12
#define PIN_I2S_BCLK  GPIO_NUM_13
#define PIN_I2S_WS    GPIO_NUM_14
#define PIN_I2S_DIN   GPIO_NUM_15   // ES7210 → 芯片（麦克风）
#define PIN_I2S_DOUT  GPIO_NUM_16   // 芯片 → ES8311（喇叭）
#define PIN_LED       GPIO_NUM_38
#define PIN_BOOT_KEY  GPIO_NUM_0
#define LED_COUNT     7
#define EXIO_PA       IO_EXPANDER_PIN_NUM_8                   // 功放使能，高有效
#define EXIO_KEYS     (IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 | IO_EXPANDER_PIN_NUM_11)  // K1/K2/K3，按下为低

#define SAMPLE_RATE   16000
#define MAX_REC_SEC   10
#define MIN_REC_MS    600
#define VOLUME        80
#define POLL_MS       30000         // 查视频进度的间隔（直连时每个在做的视频一次 HTTPS 请求）
#define READY_SHOW_MS (3 * 60 * 1000)

// ---------- 预置提示语（voice/out/*.wav，编译时打包进固件） ----------
#define VOICE(name) extern const uint8_t name##_wav_start[] asm("_binary_" #name "_wav_start"); \
                    extern const uint8_t name##_wav_end[] asm("_binary_" #name "_wav_end");
VOICE(boot) VOICE(got) VOICE(found) VOICE(not_heard) VOICE(rejected) VOICE(busy) VOICE(no_credit)
VOICE(bad_key) VOICE(net_error) VOICE(no_wifi) VOICE(ready) VOICE(failed) VOICE(setup) VOICE(saved)
#define V(name) {#name, name##_wav_start, name##_wav_end}
typedef struct { const char *id; const uint8_t *start, *end; } voice_t;
static const voice_t VOICES[] = {
  V(boot), V(got), V(found), V(not_heard), V(rejected), V(busy), V(no_credit),
  V(bad_key), V(net_error), V(no_wifi), V(ready), V(failed), V(setup), V(saved),
};

// ---------- 状态 ----------
volatile state_t g_state = ST_BOOT;
static volatile float g_level;      // 录音音量 0..1，灯环用
static char g_mac[18];

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_exio;
static esp_codec_dev_handle_t s_spk, s_mic;
static led_strip_handle_t s_strip;
static EventGroupHandle_t s_wifi_ev;
static SemaphoreHandle_t s_say_lock;
#define WIFI_UP BIT0

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ================= 灯环 =================
static void hsv(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.f, 2) - 1)), m = v - c, R = 0, G = 0, B = 0;
  int i = ((int)(h / 60)) % 6;
  if (i == 0) { R = c; G = x; } else if (i == 1) { R = x; G = c; } else if (i == 2) { G = c; B = x; }
  else if (i == 3) { G = x; B = c; } else if (i == 4) { R = x; B = c; } else { R = c; B = x; }
  *r = (R + m) * 255; *g = (G + m) * 255; *b = (B + m) * 255;
}

static void led_task(void *arg) {
  const float MAXB = 0.35f;  // 亮度上限，太亮会刺眼
  for (uint32_t t = 0;; t += 30) {
    float ph = t / 1000.f;
    for (int i = 0; i < LED_COUNT; i++) {
      uint8_t r = 0, g = 0, b = 0;
      switch (g_state) {
        case ST_BOOT:    { float v = (i <= (t / 120) % LED_COUNT) ? 0.3f : 0; r = g = b = v * 255 * MAXB; break; }
        case ST_IDLE:    { float v = 0.15f + 0.85f * (0.5f + 0.5f * sinf(ph * 2 * M_PI / 4)); hsv(28, 0.9f, v * MAXB * 0.6f, &r, &g, &b); break; }
        case ST_LISTEN:  { float v = 0.35f + 0.65f * fminf(1, g_level * 6); hsv(210, 1, v * MAXB, &r, &g, &b); break; }
        case ST_THINK:   { float head = fmodf(ph * 8, LED_COUNT); float d = fmodf(head - i + LED_COUNT, LED_COUNT);
                           hsv(275, 0.9f, MAXB * fmaxf(0.05f, 1 - d / 3), &r, &g, &b); break; }
        case ST_SPEAK:   { float v = 0.6f + 0.4f * sinf(ph * 2 * M_PI * 2); hsv(140, 0.9f, v * MAXB, &r, &g, &b); break; }
        case ST_READY:   hsv(fmodf(ph * 90 + i * 360.f / LED_COUNT, 360), 1, MAXB, &r, &g, &b); break;
        case ST_OFFLINE: { float v = ((t / 500) % 2) ? MAXB * 0.6f : 0; hsv(0, 1, v, &r, &g, &b); break; }
        case ST_SETUP:   { float v = 0.2f + 0.8f * (0.5f + 0.5f * sinf(ph * 2 * M_PI / 2)); hsv(180, 0.8f, v * MAXB, &r, &g, &b); break; }
      }
      led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

static void led_init(void) {
  led_strip_config_t cfg = {
    .strip_gpio_num = PIN_LED, .max_leds = LED_COUNT, .led_model = LED_MODEL_WS2812,
    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  // 微雪官方示例用 RGB 顺序
  };
  led_strip_rmt_config_t rmt = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000 };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
  led_strip_clear(s_strip);
  xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
}

// ================= I2C / 扩展 IO / 按键 =================
static void board_init(void) {
  i2c_master_bus_config_t bus = {
    .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
  ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca95xx_16bit(s_i2c, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &s_exio));
  // 只动功放脚；EXIO7（USB/串口切换）保持默认，不然 USB 下载口会断
  ESP_ERROR_CHECK(esp_io_expander_set_dir(s_exio, EXIO_PA, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(s_exio, EXIO_PA, 0));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(s_exio, EXIO_KEYS, IO_EXPANDER_INPUT));

  gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_BOOT_KEY, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
  gpio_config(&io);
}

/** K1/K2/K3 有没有按下（不含 BOOT：开机时按住 BOOT 会进下载模式） */
static bool exio_key_down(void) {
  uint32_t lv = 0xffffffff;
  if (esp_io_expander_get_level(s_exio, EXIO_KEYS, &lv) != ESP_OK) return false;
  return (lv & EXIO_KEYS) != EXIO_KEYS;
}

/** 任意键按下：BOOT 或 K1/K2/K3 */
static bool key_down(void) { return gpio_get_level(PIN_BOOT_KEY) == 0 || exio_key_down(); }

// ================= 音频：ES8311 喇叭 + ES7210 麦克风 =================
static void audio_init(void) {
  i2s_chan_handle_t tx, rx;
  i2s_chan_config_t chan = { .id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 240,
                             .auto_clear_after_cb = true };
  ESP_ERROR_CHECK(i2s_new_channel(&chan, &tx, &rx));
  i2s_std_config_t std = {
    .clk_cfg = { .sample_rate_hz = SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
    .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                  .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = 16,
                  .bit_shift = true, .left_align = true },
    .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED },
  };
  i2s_tdm_config_t tdm = {
    .clk_cfg = { .sample_rate_hz = SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8 },
    .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                  .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                  .ws_width = I2S_TDM_AUTO_WS_WIDTH, .bit_shift = true, .total_slot = I2S_TDM_AUTO_SLOT_NUM },
    .gpio_cfg = { .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_I2S_DIN },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
  ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm));
  ESP_ERROR_CHECK(i2s_channel_enable(tx));
  ESP_ERROR_CHECK(i2s_channel_enable(rx));

  audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

  audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
  const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
  const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
  es8311_codec_cfg_t es8311 = { .ctrl_if = out_ctrl, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                                .pa_pin = -1, .use_mclk = true,
                                .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 } };
  esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311), .data_if = data_if };
  s_spk = esp_codec_dev_new(&out_cfg);

  i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
  es7210_codec_cfg_t es7210 = { .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg),
                                .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4 };
  esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210), .data_if = data_if };
  s_mic = esp_codec_dev_new(&in_cfg);

  esp_codec_dev_sample_info_t out_fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = SAMPLE_RATE };
  ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &out_fs));
  esp_codec_dev_set_out_vol(s_spk, VOLUME);
  // TDM 4 路里只取第 0 路（MIC1），读出来就是单声道
  esp_codec_dev_sample_info_t in_fs = { .bits_per_sample = 16, .channel = 4,
                                        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), .sample_rate = SAMPLE_RATE };
  ESP_ERROR_CHECK(esp_codec_dev_open(s_mic, &in_fs));
  esp_codec_dev_set_in_channel_gain(s_mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0);
}

static void play_pcm(const int16_t *pcm, size_t samples) {
  static const int16_t silence[480] = {0};
  esp_io_expander_set_level(s_exio, EXIO_PA, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  for (size_t off = 0; off < samples; off += 512) {
    size_t n = samples - off < 512 ? samples - off : 512;
    esp_codec_dev_write(s_spk, (void *)(pcm + off), n * 2);
  }
  for (int i = 0; i < 6; i++) esp_codec_dev_write(s_spk, (void *)silence, sizeof silence);  // 冲掉 DMA 尾巴，免得爆音
  esp_io_expander_set_level(s_exio, EXIO_PA, 0);
}

/** 播一句预置提示语（WAV：找 data 块）。配网页保存时会从别的任务调用，所以加锁 */
void say(const char *id) {
  if (!id) return;
  xSemaphoreTake(s_say_lock, portMAX_DELAY);
  state_t prev = g_state;
  if (g_state != ST_SETUP) g_state = ST_SPEAK;
  bool played = false;
  for (size_t k = 0; k < sizeof VOICES / sizeof VOICES[0] && !played; k++) {
    if (strcmp(VOICES[k].id, id)) continue;
    const uint8_t *p = VOICES[k].start + 12, *e = VOICES[k].end;
    while (p + 8 <= e) {
      uint32_t sz = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
      if (!memcmp(p, "data", 4)) {
        if (sz > (uint32_t)(e - p - 8)) sz = e - p - 8;
        ESP_LOGI(TAG, "播放 %s", id);
        play_pcm((const int16_t *)(p + 8), sz / 2);
        played = true;
        break;
      }
      p += 8 + sz + (sz & 1);
    }
  }
  if (!played) ESP_LOGW(TAG, "找不到提示语 %s", id);
  g_state = prev;
  xSemaphoreGive(s_say_lock);
}

/** 按住期间录音，返回采样数 */
static size_t record(int16_t *buf, size_t max_samples) {
  int16_t chunk[320];  // 20ms
  for (int i = 0; i < 5; i++) esp_codec_dev_read(s_mic, chunk, sizeof chunk);  // 丢掉缓冲里的旧数据
  size_t n = 0;
  int up_ms = 0;
  while (n + 320 <= max_samples) {
    esp_codec_dev_read(s_mic, chunk, sizeof chunk);
    memcpy(buf + n, chunk, sizeof chunk);
    n += 320;
    float acc = 0;
    for (int i = 0; i < 320; i++) acc += (float)chunk[i] * chunk[i];
    g_level = sqrtf(acc / 320) / 32768.f;
    // 松手后再录 200ms，免得把最后一个字截掉
    if (!key_down()) { if ((up_ms += 20) >= 200) break; } else up_ms = 0;
  }
  g_level = 0;
  return n;
}

// ================= WiFi =================
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (g_state == ST_SETUP) return;  // 配网时 STA 只用来扫描
  if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
    xEventGroupClearBits(s_wifi_ev, WIFI_UP);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "WiFi 已连接 " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
    xEventGroupSetBits(s_wifi_ev, WIFI_UP);
  }
}

static void wifi_init(void) {
  s_wifi_ev = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_mac, sizeof g_mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_connect(void) {
  wifi_config_t wc = {0};
  strlcpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof wc.sta.ssid);
  strlcpy((char *)wc.sta.password, g_cfg.pass, sizeof wc.sta.password);
  wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);  // 要响应快，不省电
  ESP_LOGI(TAG, "设备 %s，连接 WiFi「%s」", g_mac, g_cfg.ssid);
}

static bool online(void) { return xEventGroupGetBits(s_wifi_ev) & WIFI_UP; }

// ================= 主流程 =================
static void handle_question(int16_t *rec) {
  g_state = ST_LISTEN;
  int64_t t0 = now_ms();
  size_t n = record(rec, SAMPLE_RATE * MAX_REC_SEC);
  if (now_ms() - t0 < MIN_REC_MS) { g_state = ST_IDLE; return; }  // 误触
  if (!online()) { say("no_wifi"); g_state = ST_OFFLINE; return; }

  g_state = ST_THINK;
  const char *prompt = api_ask(rec, n);
  memset(rec, 0, n * 2);  // 录音用完就清掉
  g_state = ST_IDLE;
  say(prompt);
}

/** 开机时按住 K1/K2/K3 两秒：进配网 */
static bool boot_hold(void) {
  int64_t t0 = now_ms();
  while (exio_key_down()) {
    if (now_ms() - t0 > 2000) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
  s_say_lock = xSemaphoreCreateMutex();

  board_init();
  led_init();
  audio_init();
  settings_load();
  wifi_init();

  if (!settings_ready() || boot_hold()) portal_run();  // 不返回：保存后重启

  api_init(g_mac);
  wifi_connect();
  int16_t *rec = heap_caps_malloc(SAMPLE_RATE * MAX_REC_SEC * 2, MALLOC_CAP_SPIRAM);
  assert(rec);

  bool up = xEventGroupWaitBits(s_wifi_ev, WIFI_UP, false, true, pdMS_TO_TICKS(20000)) & WIFI_UP;
  g_state = up ? ST_IDLE : ST_OFFLINE;
  say(up ? "boot" : "no_wifi");

  int64_t last_poll = up ? now_ms() - POLL_MS + 5000 : 0, ready_since = 0;
  for (;;) {
    if (key_down()) {
      // 没网时按一下按键：进配网（多半是换了 WiFi 或密码）
      if (g_state == ST_OFFLINE) portal_run();
      handle_question(rec);
      while (key_down()) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (g_state == ST_OFFLINE && online()) g_state = ST_IDLE;
    if (g_state != ST_OFFLINE && !online()) g_state = ST_OFFLINE;
    if (g_state == ST_READY && now_ms() - ready_since > READY_SHOW_MS) g_state = ST_IDLE;

    if ((g_state == ST_IDLE || g_state == ST_READY) && now_ms() - last_poll > POLL_MS) {
      last_poll = now_ms();
      // 直连时没有在做的视频就不用查
      const char *what = settings_direct() && api_pending() == 0 ? NULL : api_check();
      if (what) {
        say(what);
        if (strcmp(what, "ready") == 0) { g_state = ST_READY; ready_since = now_ms(); }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
