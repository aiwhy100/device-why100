# device-why100

**[English](README.en.md)** · 中文

**让孩子按一下按钮问「为什么」，几分钟后在爸爸妈妈的手机上看到一条科普动画。**

这是 [AI为什么100](https://aiwhy100.com) 的硬件接入工具包：ESP32 固件、Arduino / Python 示例、自建网关、本地模拟服务器、小智 AI 接入，全部开源（Apache-2.0）。

```
孩子按住按钮：「为什么天上的星星会眨眼睛？」
   ↓  语音识别（AI为什么100 的识别接口，或自己部署的 asr-server）
   ↓  提交问题 → AI 写稿、核对事实、画动画、配音（通常约 10 分钟）
设备：「你问的视频做好啦，请爸爸妈妈在手机上看看吧！」
家长手机：一条约 100 秒、讲给 5–12 岁孩子听的竖屏科普动画
```

成片长什么样：[aiwhy100.com/videos](https://aiwhy100.com/videos/)

## 选一种接法

| 你是 | 推荐接法 | 要准备的 | 从这里开始 |
|---|---|---|---|
| 创客 / 个人开发者 / 家长 DIY | **直连**：设备直接调 AI为什么100 | 一个 API Key | [5 分钟上手](#5-分钟上手) |
| 已经有小智 AI（xiaozhi-esp32）设备 | **小智 MCP**：对小智说「帮我做个视频」 | API Key + 一条命令 | [docs/xiaozhi.md](docs/xiaozhi.md) |
| 硬件 / 学习机 / 早教厂商 | **自建网关**：设备 → 你们的服务器 → AI为什么100 | 合作厂商 Key（联系我们开通） | [docs/gateway.md](docs/gateway.md) |

不确定选哪个？先用直连跑通，之后换成网关，设备端几乎不用改。

## 5 分钟上手

### 1. 拿一个 API Key

在手机或电脑上打开 <https://aiwhy100.com/app/>，注册后进入「设置 → API Key」新建一个（明文只显示一次，存好）。新账号送 1 条额度。

> **不想花额度？** 用本仓库的模拟服务器把整个流程跑通，接口和线上一模一样：
>
> ```bash
> node mock/server.mjs                                  # http://127.0.0.1:8787，视频 30 秒「做好」
> export AIWHY100_BASE_URL=http://127.0.0.1:8787        # 下面的例子都会改连模拟服务器
> ```

### 2. 用 curl 问第一个问题

```bash
export AIWHY100_API_KEY=svf_live_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
sh examples/curl/quickstart.sh "为什么天空是蓝色的？"
```

### 3. 用 Python（树莓派 / 电脑，只用标准库）

```bash
python3 examples/python/why.py "为什么月亮会跟着我走？"   # 文字
python3 examples/python/why.py --wav q.wav                 # 录音文件 → 识别 → 提问
python3 examples/python/why.py --loop                      # 树莓派 + USB 麦克风：按回车录音
```

### 4. 上板子

| 板子 | 用什么 | 说明 |
|---|---|---|
| **微雪 ESP32-S3-AUDIO-Board**（自带双麦、喇叭、灯环、按键） | [固件](firmware/esp32s3-audio-board) | 完整体验：按键说话、提示音、灯效、手机配网，不用写代码 |
| 任意 ESP32-S3 + INMP441 麦克风（约 10 元） | [Arduino 示例](examples/arduino/why100_esp32) | Arduino IDE 打开就能传；没接麦克风也能在串口里打字提问 |
| 树莓派 + USB 麦克风 | [Python 示例](examples/python/why.py) | `--loop` 模式像一个按钮 |
| 小智 AI 设备 | [小智接入](docs/xiaozhi.md) | 不用改固件 |

## 微雪 ESP32-S3-AUDIO-Board 固件

**不用写代码**：刷好固件后，用手机配网就能用。

1. **刷固件**：从 [Releases](https://github.com/aiwhy100/device-why100/releases) 下载 `why100-esp32s3-audio-board.bin`，用 USB-C 线连电脑：

   ```bash
   pip install esptool
   esptool.py --chip esp32s3 write_flash 0x0 why100-esp32s3-audio-board.bin
   ```

   或者用电脑上的 Chrome / Edge 打开[网页刷机页](https://aiwhy100.github.io/device-why100/)，点一下就能刷，什么都不用装。
   想自己编译：见 [docs/firmware.md](docs/firmware.md)。
2. **配网**：第一次开机，板子会说「请爸爸妈妈打开手机的无线网设置……」，灯环青色呼吸。手机连上 `AI为什么100-xxxx` 热点，会自动弹出设置页（没弹出就打开 `http://192.168.4.1`），填 WiFi（2.4G）和 API Key，保存。
3. **提问**：板子说「我准备好啦」后，按住任意按键说话，松开。做好后板子会说「你问的视频做好啦」，灯环变彩虹。
4. **看视频**：手机打开 <https://aiwhy100.com/app/>，登录同一个账号，在「我的视频」里看。

| 灯环 | 意思 |
|---|---|
| 暖橙呼吸 | 待机，可以提问 |
| 蓝色（随声音变亮） | 正在听 |
| 紫色转圈 | 正在想（识别、提交） |
| 绿色 | 正在说话 |
| 彩虹 | 视频做好了（3 分钟后或按键后恢复） |
| 红色闪 | 没网（按一下按键进入配网） |
| 青色呼吸 | 配网中 |

要重新设置（换 WiFi / 换 Key / 改成连网关）：开机时按住 K1、K2 或 K3 两秒。

## 仓库里有什么

```
firmware/esp32s3-audio-board/   ESP-IDF 固件：按键说话、提示音、灯环、手机配网、直连 / 网关两种接法
examples/
  curl/quickstart.sh            一分钟上手
  python/why.py                 树莓派 / 电脑：文字、录音文件、现场录音
  arduino/why100_esp32/         Arduino：ESP32-S3 + INMP441，或只用串口
gateway/                        自建网关（Node.js 零依赖 + Docker）：设备接口、家长手机网页、回调验签
mock/server.mjs                 本地模拟服务器：不花额度联调（个人接口 / 厂商接口 / MCP）
integrations/xiaozhi/           小智 AI MCP 接入（零依赖转发脚本）
voice/                          设备提示语（吉祥物「小奇」的声音）及生成脚本
docs/                           接口、固件、网关、小智、隐私
```

识别服务单独开源在 **[asr-server](https://github.com/aiwhy100/asr-server)**：离线中文识别，和线上接口同协议，厂商可以自己部署，录音不出机房。

## 接口速览

完整说明：[docs/api.md](docs/api.md)。地址 `https://api.aiwhy100.com/api/app/v1`，请求头 `Authorization: Bearer <API Key>`。

| 接口 | 作用 |
|---|---|
| `POST /asr?format=pcm16` | 录音（16kHz 单声道 16 位）→ `{ question }`，最长 15 秒 |
| `POST /questions/match` | 这个问题「小奇讲过啦」？讲过的直接收进账号，**不扣额度** |
| `POST /videos` | 提交问题 `{ question, channel: "kids" }`，扣 1 条额度 |
| `GET /videos/:id` | 查进度：`queued` 思考中 → `producing` 制作中 → `done`，完成后带播放链接 |

个人账号的限制：同时最多 2 个在做、每小时最多提交 5 个；问题不合适（422）、制作失败都不扣额度。

## 给孩子用的产品，请注意

- **隐私**：录音只用来识别成文字，识别完就丢，不要保存；只上传问题文字，不传姓名、手机号等个人信息。面向儿童的产品要有隐私政策并取得家长同意。见 [docs/privacy.md](docs/privacy.md)。
- **内容**：我们在提交时和制作前两道把关，不适合孩子的问题直接拒绝（不扣额度），设备收到 422 时温和地请孩子换一个问题。
- **标注**：视频由 AI 生成，播放的地方请标注「AI 生成」。

## 常见问题

**板子一直说「连不上网」？** 只支持 2.4G WiFi；密码区分大小写。按一下按键重新配网。

**提示「设置好像不对」？** API Key 错了或被删了，开机时按住 K1 两秒重新设置。

**提示「提问的次数用完啦」？** 在 aiwhy100.com/app 里看看额度。

**视频要多久？** 通常约 10 分钟，排队时会长一些。同一个问题如果「小奇讲过啦」，马上就有。

**能接别的板子吗？** 能。接口就是普通 HTTPS + JSON，参考 [Arduino 示例](examples/arduino/why100_esp32) 或 [docs/firmware.md「移植到别的板子」](docs/firmware.md#移植到别的板子)。做出来了欢迎提 PR 加到列表里。

## 参与

欢迎提 Issue / PR：新板子的适配、其他语言的示例、文档改进。提交 PR 前请跑 `npm test`。

## 许可

[Apache-2.0](LICENSE)。提示语音频的来源见 [NOTICE](NOTICE)。

由 [AI为什么100](https://aiwhy100.com) 开源。问题反馈：support@aiwhy100.com；厂商合作：同一邮箱。
