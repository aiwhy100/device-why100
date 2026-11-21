# 设备接口（个人开发者）

**[English](api.en.md)** · 中文

设备直连 AI为什么100 用到的全部接口。合作厂商（服务端对服务端、带回调和 `requestId` 幂等）请看[厂商接入文档](https://admin.aiwhy100.com/docs/partner-api)和 [gateway.md](gateway.md)。

| 项目 | 值 |
|---|---|
| 地址 | `https://api.aiwhy100.com/api/app/v1` |
| 鉴权 | 请求头 `Authorization: Bearer svf_live_…`。在 <https://aiwhy100.com/app/>「设置 → API Key」创建，明文只显示一次 |
| 格式 | JSON（录音接口除外），UTF-8 |
| 本地联调 | `node mock/server.mjs` 后把地址换成 `http://127.0.0.1:8787/api/app/v1`，Key 随便填 |

**Key 的安全**：Key 能花你账号的额度（但不能买东西、不能再建 Key）。写进设备 NVS / 配置文件，不要写死在源码里提交；设备丢了就在设置页删掉对应的 Key，立即失效。一台设备一个 Key，方便单独吊销。

## 推荐流程

```
按下按钮 → 录音（16kHz 单声道 16 位，最长 15 秒）
松开     → POST /asr?format=pcm16                → question（空串 = 没听清）
         → POST /questions/match {question}       → 有：小奇讲过啦，POST /videos/:id/save，马上可以看（不扣额度）
         → 没有：POST /videos {question, channel:"kids"} → id，告诉孩子「做好了告诉你」
之后     → 每 20–30 秒 GET /videos/:id            → status=done：播报「做好啦」；failed：请孩子换个问题
```

一条视频通常约 10 分钟，设备端要设计成「提交后可以走开」，不要阻塞等待。

## `POST /asr?format=pcm16|opus` 语音识别

请求体是整段录音，`Content-Type: application/octet-stream`，最大 1 MB、15 秒。

- `pcm16`：16kHz、单声道、16 位小端 PCM，裸数据（不要 WAV 头）。ESP32 直接发这个。
- `opus`：手机 / 弱网用，格式见 [asr-server 协议](https://github.com/aiwhy100/asr-server/blob/main/docs/protocol.md)。还有 WebSocket 流式接口（边说边出字），同一份文档。

```bash
curl -s "$BASE/asr?format=pcm16" -H "Authorization: Bearer $KEY" \
  -H 'Content-Type: application/octet-stream' --data-binary @q.pcm
# {"text":"为什么天上的星星会眨眼睛呀","question":"为什么天上的星星会眨眼睛呀？","audioMs":5088,"truncated":false}
```

`question` 已经去掉了「嗯」「我想问一下」等，末尾补了问号，可以直接提交；**空串表示没听清**，请孩子再说一遍。识别不扣额度，每分钟最多 30 句。录音识别完立即丢弃，不保存。

## `POST /questions/match` 小奇讲过啦？

```json
{ "question": "为什么天空是蓝色的？" }
```

返回 `{ "match": null }`，或：

```json
{ "match": { "own": false, "video": { "id": "cmh…", "title": "天空为什么是蓝色的", "status": "done", … } } }
```

- `own: true`：这个账号自己问过，直接用 `video`。
- `own: false`：别人问过、已经做好的成片（「小奇讲过啦」）。调 `POST /videos/:id/save` 收进这个账号的「我的视频」，**不扣额度**，马上就能看。

只查不扣额度。设备上没有「不是这个」的按钮时，命中就直接收进来即可。

## `POST /videos` 提交问题

```json
{ "question": "为什么月亮会跟着我走？", "channel": "kids" }
```

| 字段 | 说明 |
|---|---|
| `question` | 4–120 字，要是一个问句（含「为什么」等疑问词，或以问号结尾） |
| `channel` | `kids`：给 5–12 岁孩子（约 100 秒，结尾有「你可以试试」小实验和一个新问题），**设备一般用这个**；`web`：普通（60–90 秒） |
| `style` | 可选：`手绘卡通`（默认）、`3Blue1Brown 式数学动画`、`平面设计 / 信息图`、`动态文字` |

返回视频对象（见下），扣 1 条额度。

## `GET /videos/:id` 查进度

返回视频对象。建议思考中 / 制作中每 20–30 秒查一次。

```json
{
  "id": "cmhq8x2k70003",
  "question": "为什么月亮会跟着我走？",
  "title": "月亮为什么跟着我走",
  "channel": "kids",
  "status": "done",
  "statusText": "已完成",
  "etaMinutes": null,
  "summary": "月亮离我们特别远……",
  "durationSec": 101,
  "failReason": null,
  "refunded": false,
  "links": {
    "video": "https://media.aiwhy100.com/…final.mp4?exp=…&sig=…",
    "cover3x4": "https://…",
    "subtitles": "https://…",
    "download": "https://…",
    "share": "https://…"
  },
  "createdAt": "2026-11-21T02:00:00.000Z",
  "finishedAt": "2026-11-21T02:10:30.000Z"
}
```

| `status` | `statusText` | 说明 |
|---|---|---|
| `queued` | 思考中 | `etaMinutes` 预计还要几分钟 |
| `producing` | 制作中 | |
| `done` | 已完成 | `links` 可用：播放 / 下载 / 字幕 **1 小时**有效，`share`（给家长的分享页）**24 小时**有效；过期了重新 GET 一次就有新的 |
| `failed` | 失败（已退还额度） | `failReason` 可以直接读给孩子听 |
| `cancelled` | 已取消（已退还额度） | |

视频是 1080×1920 竖屏 MP4（H.264 + AAC，带字幕）。没有屏幕的设备：让家长在 <https://aiwhy100.com/app/> 登录同一个账号看「我的视频」，或者把 `links.share` 推到家长手机（企业微信 / 飞书机器人、Bark 等）。

## 其他

| 接口 | 说明 |
|---|---|
| `GET /videos?limit=20` | 我的视频，新的在前 |
| `DELETE /videos/:id` | 取消还没开始制作的（`queued`），退还额度；已经开始制作返回 409 |
| `GET /me` | 账号和额度：`credits.available` 剩余条数 |
| `GET /config` | `asr: true` 表示识别接口可用（不用登录） |

## 错误

统一结构：`{ "statusCode": 422, "error": "HttpError", "message": "中文原因" }`。`message` 可以直接给用户看。

| 状态码 | 场景 | 设备上建议 |
|---|---|---|
| 400 | 参数不对 | 检查代码 |
| 401 | Key 无效 / 已删除 | 「我的设置好像不对，请爸爸妈妈帮我重新设置」 |
| 402 | 额度不足 | 「提问的次数用完啦，请爸爸妈妈帮我看看」 |
| 404 | 视频不存在 | 从本地列表里删掉 |
| 422 | 问题不适合做成视频（太短、不是科学问题、不适合孩子等），**不扣额度** | 「这个问题我们换一个吧」 |
| 429 | 太频繁：**同时最多 2 个在做、每小时最多提交 5 个**；识别每分钟 30 句 | 「我还在做前面的视频呢，等做好了再问我吧」 |
| 503 | 识别暂时不可用 | 「网络好像有点问题，等一下再试试」 |

设备上的现成提示语音频在 [voice/out](../voice/out)，对应关系见固件 `api.c`。

## MCP

同一个 Key 也可以用 MCP（给大模型 / 智能体用）：`https://api.aiwhy100.com/mcp`，Streamable HTTP，工具 `create_why_video`、`get_video`、`list_my_videos`、`get_my_quota`。小智 AI 见 [xiaozhi.md](xiaozhi.md)。
