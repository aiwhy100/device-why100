# 固件：微雪 ESP32-S3-AUDIO-Board

代码在 [firmware/esp32s3-audio-board](../firmware/esp32s3-audio-board)，ESP-IDF 5.5。

**板子**：[微雪 ESP32-S3-AUDIO-Board](https://www.waveshare.net/wiki/ESP32-S3-AUDIO-Board)：ESP32-S3（16 MB Flash + 8 MB PSRAM）、ES7210 四麦阵列、ES8311 + 功放 + 喇叭、7 颗 WS2812 灯环、K1/K2/K3 三个按键（经 TCA9555 扩展），USB-C 供电和下载。买回来插上就能用，不用焊接。

## 刷现成的固件（不用装开发环境）

从 [Releases](https://github.com/aiwhy100/device-why100/releases) 下载 `why100-esp32s3-audio-board.bin`（已经合并好 bootloader、分区表和程序），USB-C 连电脑：

```bash
pip install esptool
esptool.py --chip esp32s3 write_flash 0x0 why100-esp32s3-audio-board.bin
```

也可以用电脑上的 Chrome / Edge 打开[网页刷机页](https://aiwhy100.github.io/device-why100/)，不用装 esptool。

找不到串口时：按住板子上的 BOOT 键，再按一下 RESET，松开 BOOT，进入下载模式后重试。刷完按一下 RESET。

## 第一次使用：手机配网

板子没有屏幕，配网的每一步都有语音提示，手机页面上也有进度：

| 步骤 | 你做什么 | 板子说 / 页面显示 |
|---|---|---|
| 1 | 开机 | 「请爸爸妈妈打开手机的无线网设置，连上 AI为什么100 开头的网络……」，灯环青色呼吸；没人连时每 2 分钟提醒一次 |
| 2 | 手机连上 `AI为什么100-xxxx` 热点（不用密码） | 「手机连上我啦！请在弹出的页面里选好无线网，填上密码。」手机一般会自动弹出设置页；没弹出就在浏览器打开 `http://192.168.4.1` |
| 3 | ① 点选你家的 WiFi（**只支持 2.4G**）② 填密码 ③ 填 API Key（<https://aiwhy100.com/app/>「设置 → API Key」），点底部「保存并连接」 | 「收到，我来试试连上网。」页面显示「正在连接 WiFi…」 |
| 4 | 等 10–30 秒 | 成功：「设置好啦」，页面变绿，板子重启后说「我准备好啦」<br>WiFi 不对：「连不上这个无线网……」，页面写明原因（找不到 / 密码不对 / 信号弱）<br>Key 不对：「我的设置好像不对……」 |

板子会**先用新设置试一次**（连 WiFi + 调一次 `GET /me` 或网关 `/poll`），成功才保存；失败不保存，改好直接再点一次。

连自建网关：在第 ③ 步展开「高级」，选「自建网关」，填网关地址（如 `http://192.168.1.10:8080`），网关设了 `DEVICE_TOKENS` 时再填设备令牌。见 [gateway.md](gateway.md)。

板子连 WiFi 试设置时，有些手机会和热点断开一下，页面收不到结果；这时听板子说话就行。热点 15 分钟没人保存会自动重启。设置保存在 NVS 里，重刷固件不会丢（除非整片擦除）。

**重新设置**：开机时按住 K1 / K2 / K3 任意一个两秒；或者没网（红灯闪）时按一下按键。

## 使用

- **按住任意按键说话，松开提交**。最长 10 秒，短于 0.6 秒当作误触。
- 板子马上回一句：「好问题！我这就去做个视频」/「这个问题我讲过啦」/「我没听清楚」/「这个问题我们换一个吧」……
- 做好后（通常约 10 分钟）板子说「你问的视频做好啦，请爸爸妈妈在手机上看看吧」，灯环变彩虹。
- 家长在 <https://aiwhy100.com/app/> 登录同一个账号看（直连），或打开网关的家长网页（网关）。

全部提示语见 [voice/prompts.json](../voice/prompts.json)。

## 自己编译

需要 [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5/esp32s3/get-started/)（国内装：`IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets ./install.sh esp32s3`）。

```bash
. ~/esp/esp-idf-v5.5/export.sh
cd firmware/esp32s3-audio-board
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor      # Windows：-p COM3
```

开发时不想每次配网，可以在 `idf.py menuconfig` →「AI为什么100」里填 WiFi、接入方式和 Key 作为默认值（写进 `sdkconfig`，已在 `.gitignore` 里，不会提交）。手机配网保存过的值优先。

要从头配网：`idf.py erase-flash` 后重新刷。

## 代码结构

| 文件 | 内容 |
|---|---|
| `main/main.c` | 板子初始化（I2C、扩展 IO、ES8311 / ES7210、灯环）、录音、播放提示语、主循环 |
| `main/api.c` | 两种接入方式：直连（识别 → 查重 → 提交 → 轮询，在做的视频 ID 存 NVS）和网关 |
| `main/portal.c` | 手机配网：热点 + 假 DNS（让手机弹出设置页）+ 设置网页 |
| `main/settings.c` | 设置的读写（NVS，menuconfig 默认值） |
| `main/Kconfig.projbuild` | menuconfig 里的默认值 |
| `../../voice/out/*.wav` | 提示语，编译时打包进固件（约 1.8 MB） |

## 移植到别的板子

接口只是 HTTPS + JSON，任何能联网、能录音的板子都能接。最省事的两条路：

1. **只要一块 ESP32-S3 + 一个 INMP441 麦克风**：用 [Arduino 示例](../examples/arduino/why100_esp32)，改几个引脚就行。
2. **在这个固件上改**：
   - `main.c` 开头的引脚定义；
   - `audio_init()`：换成你的 codec。只有 I2S 数字麦克风（INMP441 等）时，直接用 `i2s_std` 读 32 位数据右移成 16 位，不需要 `esp_codec_dev`；喇叭用 MAX98357A 时同理只用 I2S 输出；
   - `key_down()`：按键怎么读；
   - `led_task()`：没有灯环就删掉，或者换成单颗 LED；
   - 没有 PSRAM 时把录音上限 `MAX_REC_SEC` 改成 4 秒（128 KB），`api.c` 的响应缓冲改用内部 RAM。

录音格式固定是 16kHz、单声道、16 位，一个请求整段上传（`POST /asr?format=pcm16`），所以不需要在设备上做编码。

适配好了新板子，欢迎提 PR 放到 `firmware/<板子名>/`。

## 隐私

录音只在内存里，上传识别后立即清零；固件不保存录音，不打印识别内容以外的个人信息。API Key 存在 NVS 里（不加密）；面向消费者量产时请开启 [NVS 加密](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5/esp32s3/api-reference/storage/nvs_encryption.html) 和 Flash 加密，或者改用网关模式（Key 不进设备）。
