# 接入小智 AI（xiaozhi-esp32）

已经有[小智 AI](https://github.com/78/xiaozhi-esp32) 设备的话，不用改固件：把 AI为什么100 加成小智的一个 MCP 工具，孩子对小智说

> 「帮我做个视频：为什么天上的星星会眨眼睛？」

小智就会调用 AI为什么100 提交问题，几分钟后家长在 <https://aiwhy100.com/app/> 看到视频。

```
小智设备 ⇄ 小智服务器 ⇄（MCP 接入点，WebSocket）⇄ bridge.mjs ⇄（HTTPS）⇄ AI为什么100 MCP
```

## 步骤

1. **拿两个东西**
   - 小智的 **MCP 接入点**：登录 [xiaozhi.me](https://xiaozhi.me) 控制台，在你的智能体的配置里找到「MCP 接入点」，复制 `wss://…?token=…` 地址。
   - AI为什么100 的 **API Key**：<https://aiwhy100.com/app/>「设置 → API Key」。
2. **运行转发脚本**（需要 Node.js 22+，零依赖）：

   ```bash
   export XIAOZHI_MCP_ENDPOINT='wss://api.xiaozhi.me/mcp/?token=…'
   export AIWHY100_API_KEY=svf_live_…
   node integrations/xiaozhi/bridge.mjs
   ```

   看到「已连上小智 MCP 接入点」就好了。控制台的接入点状态会变成在线，工具列表里出现 `create_why_video`、`get_video`、`list_my_videos`、`get_my_quota`。
3. **（建议）改一下智能体的角色介绍**，让它知道什么时候用这个工具，比如加一句：

   > 小朋友问「为什么」的科学问题，想看视频时，调用 create_why_video 把问题做成科普动画，告诉他「做好了请爸爸妈妈在手机上看」。之后问「视频做好了吗」时，调用 get_video 查一下。

## 一直开着

转发脚本要一直运行（断线会自动重连，最长间隔 5 分钟）。电脑、树莓派、NAS、云服务器都行，几乎不占资源：

```bash
# Docker
docker run -d --name xiaozhi-why100 --restart unless-stopped \
  -e XIAOZHI_MCP_ENDPOINT='wss://api.xiaozhi.me/mcp/?token=…' \
  -e AIWHY100_API_KEY=svf_live_… \
  -v "$PWD/integrations/xiaozhi:/app:ro" node:22-alpine node /app/bridge.mjs

# 或者 pm2
pm2 start integrations/xiaozhi/bridge.mjs --name xiaozhi-why100
```

## 已经在用官方 mcp_pipe.py？

小智官方的 [mcp_pipe.py](https://github.com/78/mcp-calculator) 也能接：它把 stdio 的 MCP 服务转到接入点上。AI为什么100 是远程 HTTP 的 MCP，需要再经过一层 stdio 转换，比如在它的 `mcp_config.json` 里加（字段以 mcp_pipe.py 的说明为准）：

```json
{
  "mcpServers": {
    "aiwhy100": {
      "type": "stdio",
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://api.aiwhy100.com/mcp", "--header", "Authorization:Bearer svf_live_…"]
    }
  }
}
```

`bridge.mjs` 少一层转换、不用装 Python 依赖，更推荐。

## 常见问题

**小智说「API Key 不对」？** 检查 `AIWHY100_API_KEY`，或者在设置页重新建一个。

**做好的视频在哪看？** 小智设备一般没有能播视频的屏幕：家长在 <https://aiwhy100.com/app/> 登录同一个账号，在「我的视频」里看。

**额度、频率限制？** 和直连一样：问题不合适不扣额度；同时最多 2 个在做、每小时最多 5 个。见 [api.md](api.md)。

**本地先试试（不花额度）？**

```bash
node mock/server.mjs
AIWHY100_MCP_URL=http://127.0.0.1:8787/mcp XIAOZHI_MCP_ENDPOINT='wss://…' AIWHY100_API_KEY=x node integrations/xiaozhi/bridge.mjs
```
