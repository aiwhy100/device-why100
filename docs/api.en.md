# Device API (personal developers)

English · **[中文](api.md)**

Everything a device needs to talk to AI为什么100 directly. Vendors integrating server-to-server (webhooks, `requestId` idempotency) should read the [partner API docs](https://admin.aiwhy100.com/docs/partner-api) (Chinese) and [gateway.md](gateway.md).

> Videos are produced **in Mandarin Chinese** for children aged 5–12. Questions should be in Chinese.

| Item | Value |
|---|---|
| Base URL | `https://api.aiwhy100.com/api/app/v1` |
| Auth | `Authorization: Bearer svf_live_…`. Create a key at <https://aiwhy100.com/app/> → 设置 (Settings) → API Key; it is shown once |
| Format | JSON (except the audio upload), UTF-8 |
| Local testing | `node mock/server.mjs`, then use `http://127.0.0.1:8787/api/app/v1` with any key |

**Key safety**: a key spends your account's credits (it cannot buy anything or create keys). Store it in device NVS / config, never in committed source. Delete it in Settings if a device is lost; it stops working immediately. Use one key per device.

## Recommended flow

```
button down → record (16 kHz mono 16-bit, max 15 s)
button up   → POST /asr?format=pcm16                → question ("" = nothing heard)
            → POST /questions/match {question}       → hit: already made, POST /videos/:id/save (free, instant)
            → miss: POST /videos {question, channel:"kids"} → id; tell the child "I'll let you know"
later       → GET /videos/:id every 20–30 s          → done: announce; failed: ask for another question
```

A video usually takes about 10 minutes, so design the device as fire-and-forget.

## `POST /asr?format=pcm16|opus`

Body: the whole recording, `Content-Type: application/octet-stream`, max 1 MB / 15 s.

- `pcm16`: 16 kHz mono 16-bit little-endian PCM, raw (no WAV header). ESP32 devices send this.
- `opus`: for phones / poor networks; see the [asr-server protocol](https://github.com/aiwhy100/asr-server/blob/main/docs/protocol.en.md), which also documents the streaming WebSocket.

```bash
curl -s "$BASE/asr?format=pcm16" -H "Authorization: Bearer $KEY" \
  -H 'Content-Type: application/octet-stream' --data-binary @q.pcm
# {"text":"为什么天上的星星会眨眼睛呀","question":"为什么天上的星星会眨眼睛呀？","audioMs":5088,"truncated":false}
```

`question` is cleaned up and ready to submit; **an empty string means nothing was heard**. Recognition is free (30 per minute). Audio is discarded right after recognition.

## `POST /questions/match`

`{ "question": "…" }` → `{ "match": null }` or `{ "match": { "own": bool, "video": { … } } }`.

- `own: true`: this account asked it before; use `video`.
- `own: false`: an existing finished video. `POST /videos/:id/save` adds it to the account's videos **for free**, instantly.

## `POST /videos`

`{ "question": "为什么月亮会跟着我走？", "channel": "kids" }`

| Field | Notes |
|---|---|
| `question` | 4–120 characters, a question |
| `channel` | `kids` (5–12 years, ~100 s, ends with a try-it-yourself activity and a new question), **use this for devices**; `web` (general, 60–90 s) |
| `style` | optional: `手绘卡通` (default), `3Blue1Brown 式数学动画`, `平面设计 / 信息图`, `动态文字` |

Returns the video object and uses 1 credit.

## `GET /videos/:id`

Poll every 20–30 s while `queued` / `producing`.

| `status` | Meaning |
|---|---|
| `queued` | Thinking; `etaMinutes` estimates the wait |
| `producing` | Rendering |
| `done` | `links.video` / `download` / `subtitles` valid for **1 hour**, `links.share` (a page for parents) for **24 hours**; GET again for fresh links |
| `failed` | Failed, credit refunded; `failReason` is child-friendly Chinese |
| `cancelled` | Cancelled, credit refunded |

The video is a 1080×1920 portrait MP4 (H.264 + AAC, burned-in subtitles). Screenless devices: parents watch at <https://aiwhy100.com/app/> with the same account, or push `links.share` to their phone.

## Other endpoints

| Endpoint | Notes |
|---|---|
| `GET /videos?limit=20` | My videos, newest first |
| `DELETE /videos/:id` | Cancel a `queued` video (refunded); 409 once production started |
| `GET /me` | Account and credits (`credits.available`) |
| `GET /config` | `asr: true` when speech recognition is available (no auth) |

## Errors

`{ "statusCode": 422, "error": "HttpError", "message": "<Chinese, user-facing>" }`

| Status | Cause |
|---|---|
| 400 | Bad parameters |
| 401 | Invalid or deleted key |
| 402 | Out of credits |
| 404 | No such video |
| 422 | Question not suitable (too short, not science, not for kids…), **no credit used** |
| 429 | Too many: **at most 2 videos in progress and 5 submissions per hour**; ASR 30 per minute |
| 503 | Speech recognition temporarily unavailable |

## MCP

The same key works with MCP at `https://api.aiwhy100.com/mcp` (Streamable HTTP): `create_why_video`, `get_video`, `list_my_videos`, `get_my_quota`. For xiaozhi devices see [xiaozhi.md](xiaozhi.md).
