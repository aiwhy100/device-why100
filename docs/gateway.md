# 自建网关

代码在 [gateway/](../gateway)：Node.js 20+、零依赖，自带 Docker 部署。

```
设备 ──录音──▶ 网关 ──识别──▶ AI为什么100 识别接口，或你们自己部署的 asr-server
                 │
                 ├──提交 / 查询──▶ AI为什么100（个人开发者接口 或 合作厂商接口）
                 │◀──回调（合作厂商，可选）──
                 └──家长手机网页：孩子问过的问题、做好的视频
```

**什么时候用网关**

- 你是**硬件厂商**：API Key 不能下发到设备，设备只和你们自己的服务器通信。网关就是一个可以直接用的「你们的服务器」参考实现，也可以照着它把接口加到你们现有的后台里。
- 想让**录音不出自己的机房**：网关 + 自己部署的 [asr-server](https://github.com/aiwhy100/asr-server)。
- 家里**好几台设备**共用一个账号，想要一个按设备分开的家长网页。

## 部署

### 个人开发者（Key 在网关里，识别用 AI为什么100 的）

```bash
cd gateway
cp .env.example .env      # 填 AIWHY100_API_KEY
docker compose up -d
```

不用 Docker：`node gateway/src/server.js`（环境变量同 `.env`）。

### 合作厂商（厂商接口 + 自己部署识别）

先联系我们开通合作厂商账号（support@aiwhy100.com），在厂商后台创建 Key。然后：

```bash
cd gateway
cp .env.example .env
# .env 里：
#   UPSTREAM=partner
#   AIWHY100_API_KEY=<厂商 Key>
#   ASR_URL=http://asr:8090/asr
#   ASR_TOKEN=<随机字符串，openssl rand -hex 24>
#   DEVICE_TOKENS=<随机字符串>
#   WEBHOOK_SECRET=<厂商后台「接入设置」里的 whsec_…>
docker compose --profile asr up -d     # 同时启动 asr-server，第一次会下载约 230 MB 模型
```

> `docker-compose.yml` 里的 asr-server 直接从 GitHub 仓库构建。国内服务器拉 GitHub 慢的话，先 `git clone` asr-server 到旁边，把 `build:` 改成本地路径。

然后在厂商后台「接入设置」里：回调地址填 `https://<网关域名>/webhook/aiwhy100`，IP 白名单填网关服务器的出口 IP。

### 放到公网

1. 一定要设 `DEVICE_TOKENS`（设备令牌，固件配网页里填同一个）和 `PARENT_PASSWORD`（家长网页密码）。
2. 前面加 HTTPS（nginx / Caddy），转发到 `127.0.0.1:8080`。
3. 检查：`curl https://<网关域名>/healthz` → `{"ok":true,…,"key":true}`。

## 配置

| 变量 | 默认 | 说明 |
|---|---|---|
| `UPSTREAM` | `app` | `app` 个人开发者接口 / `partner` 合作厂商接口 |
| `AIWHY100_API_KEY` | — | Key（也可以用 `AIWHY100_API_KEY_FILE` 指向一个文件） |
| `AIWHY100_BASE_URL` | `https://api.aiwhy100.com` | 本地联调时改成模拟服务器 `http://127.0.0.1:8787` |
| `CHANNEL` | `kids` | `kids` 给 5–12 岁孩子 / `web` 普通 |
| `ASR_URL` | 个人：AI为什么100 识别接口；厂商：空 | 识别地址，`POST <ASR_URL>?format=pcm16` |
| `ASR_TOKEN` | 个人：同 Key | 识别服务的令牌 |
| `DEVICE_TOKENS` | 空 | 设备令牌，逗号分隔；设了以后设备要带 `X-Device-Token` |
| `PARENT_PASSWORD` | 空 | 家长网页密码（浏览器弹窗，用户名随意） |
| `WEBHOOK_SECRET` | 空 | 合作厂商回调签名密钥；设了才接收回调 |
| `MATCH` | `true` | 先查「小奇讲过啦」（只有个人开发者接口有） |
| `PORT` / `DATA_DIR` | `8080` / `gateway/data` | 端口、数据目录（`db.json`：问题文字和视频信息，不存录音） |

## 设备接口

`<设备ID>` 一般用设备 MAC（小写带冒号，如 `24:0a:c4:12:34:56`），1–64 位字母、数字或 `. _ : @ -`。合作厂商模式下它就是厂商接口的 `externalUserId`，**不要用手机号、姓名等个人信息**。设了 `DEVICE_TOKENS` 时每个请求带 `X-Device-Token`。

| 接口 | 说明 |
|---|---|
| `POST /api/device/<设备ID>/ask` | 正文：录音，16kHz 单声道 16 位小端 PCM。返回 `{ prompt, question? }`，设备播对应的提示语 |
| `POST /api/device/<设备ID>/ask-text` | 调试用：`{ "question": "…" }`，跳过识别 |
| `GET /api/device/<设备ID>/poll` | `{ ready, ids, failed, failedIds, pending }`：做好了 / 没做成但还没播报过的 |
| `POST /api/device/<设备ID>/ack` | `{ "ids": [...] }`：播报完确认，下次 poll 就不再返回 |

`prompt` 的取值（和 [voice/out](../voice/out) 里的文件名一一对应）：

| prompt | 什么时候 |
|---|---|
| `got` | 已提交，开始做 |
| `found` | 小奇讲过啦，已经收进账号（只有个人开发者接口） |
| `not_heard` | 没听清 / 太短 |
| `rejected` | 问题不适合做成视频（不扣额度） |
| `busy` | 太频繁（个人账号同时最多 2 个在做） |
| `no_credit` | 额度用完 |
| `bad_key` | 网关的 Key 不对，或设备令牌不对 |
| `net_error` | 网关连不上 AI为什么100 / 识别服务 |

设备每 10–30 秒 `poll` 一次即可；网关自己在后台查进度（思考中 15 秒、制作中 30 秒一次；设了回调时放慢到 2 分钟兜底）。

## 回调

合作厂商模式设了 `WEBHOOK_SECRET` 后，网关在 `POST /webhook/aiwhy100` 接收 `video.done` / `video.failed`：校验 `X-SVF-Signature`（`v1=` + HMAC-SHA256(密钥, `<时间戳>.<原始请求体>`)），时间戳超过 5 分钟拒绝。厂商后台的「发送测试回调」（`ping`）会返回 200。

## 家长网页

`http://<网关>/`：只有一台设备时直接进它的列表；多台时先选设备。做好的视频点开就能播，显示小实验和新问题（合作厂商接口才有），标注「AI 生成」。封面和视频走网关的固定地址 `/c/<编号>`、`/m/<编号>`，打开时换成最新的播放链接（个人开发者接口的链接 1 小时过期）。

## 本地联调

```bash
node mock/server.mjs                                                         # 模拟 AI为什么100
AIWHY100_BASE_URL=http://127.0.0.1:8787 AIWHY100_API_KEY=x node gateway/src/server.js
curl -s localhost:8080/api/device/24:0a:c4:12:34:56/ask-text -d '{"question":"为什么月亮会跟着我走"}'
```

板子配网时把网关地址填成电脑的局域网 IP（如 `http://192.168.1.10:8080`）。
