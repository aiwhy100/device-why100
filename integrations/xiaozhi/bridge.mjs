#!/usr/bin/env node
// 把 AI为什么100 接到小智 AI（xiaozhi-esp32）：孩子对小智说「帮我做个视频，为什么天空是蓝的」，
// 小智就会调用 AI为什么100 的 create_why_video，几分钟后家长在 aiwhy100.com/app 看到视频。
//
// 小智控制台的「MCP 接入点」是一个 WebSocket，里面传的是 MCP 的 JSON-RPC 消息（小智是客户端）；
// AI为什么100 的 MCP 是 Streamable HTTP（无状态）。这个脚本在两者之间转发，零依赖，Node.js 22+。
//
//   export XIAOZHI_MCP_ENDPOINT='wss://api.xiaozhi.me/mcp/?token=…'   # 小智控制台 → 智能体 → MCP 接入点
//   export AIWHY100_API_KEY=svf_live_…                                # aiwhy100.com/app 「设置 → API Key」
//   node integrations/xiaozhi/bridge.mjs
//
// 需要一直开着（电脑、树莓派、NAS 或云服务器都行），断线会自动重连。见 docs/xiaozhi.md。
import { fileURLToPath } from 'node:url';

const ts = () => new Date().toLocaleTimeString('zh-CN', { hour12: false });

/** 把一条 JSON-RPC 消息（或批量）转给 AI为什么100 的 MCP，返回要回给小智的消息数组 */
export async function forward(msg, { mcpUrl, apiKey, fetchImpl = fetch }) {
  const res = await fetchImpl(mcpUrl, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${apiKey}`,
      'Content-Type': 'application/json',
      Accept: 'application/json, text/event-stream',
    },
    body: JSON.stringify(msg),
    signal: AbortSignal.timeout(60_000),
  });
  if (res.status === 202 || res.status === 204) return []; // 通知，不用回
  const text = await res.text();
  const ids = (Array.isArray(msg) ? msg : [msg]).filter((m) => m && m.id !== undefined).map((m) => m.id);
  if (!res.ok) {
    let message = `AI为什么100 返回 HTTP ${res.status}`;
    try {
      message = JSON.parse(text).message ?? JSON.parse(text).error?.message ?? message;
    } catch {}
    if (res.status === 401) message = 'AI为什么100 的 API Key 不对或已删除';
    return ids.map((id) => ({ jsonrpc: '2.0', id, error: { code: -32000, message } }));
  }
  const ctype = res.headers.get('content-type') ?? '';
  const out = [];
  if (ctype.includes('text/event-stream')) {
    // SSE：每个事件的 data 行是一条 JSON-RPC 消息
    for (const block of text.split(/\r?\n\r?\n/)) {
      const data = block.split(/\r?\n/).filter((l) => l.startsWith('data:')).map((l) => l.slice(5).trimStart()).join('\n');
      if (data) out.push(JSON.parse(data));
    }
  } else if (text.trim()) {
    out.push(JSON.parse(text));
  }
  return out.flat();
}

export function startBridge({ endpoint, mcpUrl, apiKey, log = (m) => console.log(ts(), m), WebSocketImpl = globalThis.WebSocket }) {
  if (!WebSocketImpl) throw new Error('需要 Node.js 22 以上（自带 WebSocket）');
  let ws;
  let stopped = false;
  let backoff = 1000;
  let timer;

  const connect = () => {
    ws = new WebSocketImpl(endpoint);
    ws.addEventListener('open', () => {
      backoff = 1000;
      log('已连上小智 MCP 接入点');
    });
    ws.addEventListener('message', async (ev) => {
      const raw = typeof ev.data === 'string' ? ev.data : Buffer.from(ev.data).toString('utf8');
      let msg;
      try {
        msg = JSON.parse(raw);
      } catch {
        return log('收到的不是 JSON，忽略');
      }
      const method = Array.isArray(msg) ? `批量 ${msg.length} 条` : msg.method ?? '响应';
      if (method === 'tools/call') log(`小智调用 ${msg.params?.name}：${JSON.stringify(msg.params?.arguments ?? {})}`);
      try {
        for (const reply of await forward(msg, { mcpUrl, apiKey })) {
          if (ws.readyState === 1) ws.send(JSON.stringify(reply));
        }
      } catch (e) {
        log(`转发失败（${method}）：${e.message}`);
        const ids = (Array.isArray(msg) ? msg : [msg]).filter((m) => m && m.id !== undefined).map((m) => m.id);
        for (const id of ids) ws.send(JSON.stringify({ jsonrpc: '2.0', id, error: { code: -32000, message: `连不上 AI为什么100：${e.message}` } }));
      }
    });
    ws.addEventListener('close', () => {
      if (stopped) return;
      log(`连接断开，${backoff / 1000} 秒后重连`);
      timer = setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 5 * 60_000);
    });
    ws.addEventListener('error', () => {}); // close 里统一重连
  };
  connect();
  return {
    stop() {
      stopped = true;
      clearTimeout(timer);
      ws?.close();
    },
  };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const endpoint = process.env.XIAOZHI_MCP_ENDPOINT;
  const apiKey = process.env.AIWHY100_API_KEY;
  const mcpUrl = process.env.AIWHY100_MCP_URL || 'https://api.aiwhy100.com/mcp';
  if (!endpoint || !apiKey) {
    console.error('需要两个环境变量：\n  XIAOZHI_MCP_ENDPOINT  小智控制台 → 智能体 → MCP 接入点（wss://…）\n  AIWHY100_API_KEY      https://aiwhy100.com/app/ 「设置 → API Key」');
    process.exit(1);
  }
  console.log(`${ts()} 转发：小智 ⇄ ${mcpUrl}`);
  startBridge({ endpoint, mcpUrl, apiKey });
}
