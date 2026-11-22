# device-why100

English · **[中文](README.md)**

**A kid presses a button and asks "why?". A few minutes later an animated science video shows up on the parent's phone.**

This is the hardware integration kit for [AI为什么100](https://aiwhy100.com): ESP32 firmware, Arduino and Python examples, a self-hosted gateway, a local mock server and a xiaozhi AI bridge. Apache-2.0.

> AI为什么100 makes **Mandarin Chinese** videos for children aged 5–12. Questions should be asked in Chinese.

```
Kid holds the button: "为什么天上的星星会眨眼睛？" (Why do stars twinkle?)
   ↓  speech recognition (AI为什么100's endpoint, or your own asr-server)
   ↓  AI writes the script, checks facts, animates and voices it (~10 minutes)
Device: "你问的视频做好啦" (Your video is ready!)
Parent's phone: a ~100-second portrait science cartoon for kids
```

Sample videos: [aiwhy100.com/videos](https://aiwhy100.com/videos/)

## Pick an integration

| You are | Use | You need | Start here |
|---|---|---|---|
| Maker / hobbyist / parent | **Direct**: the device calls AI为什么100 | An API key | [Quick start](#quick-start) |
| Owner of a xiaozhi AI (xiaozhi-esp32) device | **xiaozhi MCP** | API key + one command | [docs/xiaozhi.md](docs/xiaozhi.md) (Chinese) |
| Device / tablet / edtech vendor | **Gateway**: device → your server → AI为什么100 | A partner key (contact us) | [docs/gateway.md](docs/gateway.md) (Chinese) |

## Quick start

1. **Get an API key**: sign up at <https://aiwhy100.com/app/>, then 设置 (Settings) → API Key. New accounts get 1 free credit.
   Don't want to spend credits? Run the mock server, which speaks the same API:

   ```bash
   node mock/server.mjs                              # http://127.0.0.1:8787, videos "finish" in 30 s
   export AIWHY100_BASE_URL=http://127.0.0.1:8787
   ```

2. **curl**:

   ```bash
   export AIWHY100_API_KEY=svf_live_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
   sh examples/curl/quickstart.sh "为什么天空是蓝色的？"
   ```

3. **Python** (Raspberry Pi / desktop, standard library only):

   ```bash
   python3 examples/python/why.py "为什么月亮会跟着我走？"
   python3 examples/python/why.py --loop          # Pi + USB mic: press Enter to talk
   ```

4. **Hardware**:

   | Board | Use |
   |---|---|
   | **Waveshare ESP32-S3-AUDIO-Board** (mics, speaker, LED ring, buttons) | [Firmware](firmware/esp32s3-audio-board): push-to-talk, voice prompts, LEDs, phone-based Wi-Fi setup, no code needed |
   | Any ESP32-S3 + INMP441 mic | [Arduino sketch](examples/arduino/why100_esp32); works over the serial monitor without a mic too |
   | Raspberry Pi + USB mic | [Python example](examples/python/why.py) |
   | xiaozhi AI devices | [xiaozhi bridge](docs/xiaozhi.md), no firmware change |

## Waveshare ESP32-S3-AUDIO-Board firmware

1. **Flash**: download `why100-esp32s3-audio-board.bin` from [Releases](https://github.com/aiwhy100/device-why100/releases), then
   `pip install esptool && esptool.py --chip esp32s3 write_flash 0x0 why100-esp32s3-audio-board.bin`.
   Or open the [web flasher](https://aiwhy100.github.io/device-why100/) in desktop Chrome / Edge and flash with one click.
   To build it yourself, see [docs/firmware.md](docs/firmware.md).
2. **Set up Wi-Fi**: on first boot the board asks (in Chinese) for a parent to join its hotspot, and the LED ring breathes cyan. Join `AI为什么100-xxxx` with your phone; the board confirms out loud and a setup page pops up (or open `http://192.168.4.1`). Tap your 2.4 GHz network, enter the password and API key, then tap the button at the bottom. The board tries the settings on the spot and speaks the result (success, wrong Wi-Fi password, or bad key); the page shows progress too. Nothing is saved unless it works.
3. **Ask**: hold any button, speak, release. When the video is ready the board says so and the ring turns rainbow.
4. **Watch**: log in at <https://aiwhy100.com/app/> with the same account.

Hold K1/K2/K3 for 2 seconds at power-on to change settings.

## What's inside

```
firmware/esp32s3-audio-board/   ESP-IDF firmware: push-to-talk, prompts, LED ring, phone setup, direct or gateway mode
examples/curl | python | arduino
gateway/                        Self-hosted gateway (zero-dependency Node.js + Docker): device API, parent web page, webhooks
mock/server.mjs                 Local mock of the AI为什么100 API (personal, partner and MCP)
integrations/xiaozhi/           xiaozhi AI MCP bridge (zero dependencies)
voice/                          Device prompts (mascot "小奇") and generator
docs/                           API, firmware, gateway, xiaozhi, privacy
```

Speech recognition is open-sourced separately as **[asr-server](https://github.com/aiwhy100/asr-server)**: offline Chinese ASR with the same protocol as the hosted endpoint, so vendors can keep audio in-house.

## API at a glance

Full reference: [docs/api.en.md](docs/api.en.md). Base `https://api.aiwhy100.com/api/app/v1`, header `Authorization: Bearer <API key>`.

| Endpoint | Purpose |
|---|---|
| `POST /asr?format=pcm16` | Audio (16 kHz mono 16-bit) → `{ question }`, max 15 s |
| `POST /questions/match` | Already made? Add the existing video to the account **for free** |
| `POST /videos` | Submit `{ question, channel: "kids" }`, uses 1 credit |
| `GET /videos/:id` | `queued` → `producing` → `done` with playback links |

Personal accounts: at most 2 videos in progress and 5 submissions per hour. Rejected questions (422) and failed videos cost nothing.

## Building for kids

- **Privacy**: use audio only for recognition and discard it; send only the question text, never names or phone numbers. Get parental consent. See [docs/privacy.md](docs/privacy.md).
- **Content**: unsuitable questions are rejected with 422 (free); have the device gently ask for another question.
- **Labeling**: videos are AI-generated; label them "AI 生成" where they play.

## Contributing

Issues and PRs welcome: new boards, examples in other languages, docs. Run `npm test` before sending a PR.

## License

[Apache-2.0](LICENSE). See [NOTICE](NOTICE) for the voice prompts.

Open-sourced by [AI为什么100](https://aiwhy100.com). Feedback and partnerships: support@aiwhy100.com.
