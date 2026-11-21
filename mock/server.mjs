#!/usr/bin/env node
// 本地模拟服务器：模拟 AI为什么100 的接口，不花额度、不联网就能把设备流程跑通。
//   node mock/server.mjs                 默认 http://0.0.0.0:8787
//   MOCK_FAST=1 node mock/server.mjs     视频 3 秒做好（默认约 30 秒）
//
// 模拟的接口（路径和线上一致，见 docs/api.md）：
//   个人开发者 /api/app/v1：asr、questions/match、videos、videos/:id、videos/:id/save、me、config
//   合作厂商   /api/partner/v1：videos、videos/:id、account（支持 requestId 幂等和签名回调）
//   MCP        /mcp：create_why_video、get_video、list_my_videos、get_my_quota（Streamable HTTP，无状态，JSON 响应）
//
// 用问题里的关键词触发各种情况（方便测设备端的提示音）：
//   含「拒绝」       → 422 问题不适合（不扣额度）
//   含「失败」       → 制作失败，额度退还
//   和题库里的问题一样（如「为什么天空是蓝色的？」）→ questions/match 命中「小奇讲过啦」
//   额度用完         → 402（MOCK_CREDITS，默认 20）
//   同时在做超过 2 个 → 429（个人开发者接口的限制）
import { createHmac, randomBytes } from 'node:crypto';
import http from 'node:http';
import { fileURLToPath } from 'node:url';

const env = process.env;
const FAST = env.MOCK_FAST === '1';
const QUEUE_SEC = Number(env.MOCK_QUEUE_SEC ?? (FAST ? 1 : 8));
const DONE_SEC = Number(env.MOCK_DONE_SEC ?? (FAST ? 3 : 30));

// 线上公开的示例成片（官网每期页的固定地址，302 到带签名的媒体链接）
const SAMPLE = {
  video: 'https://aiwhy100.com/videos/17/video',
  cover: 'https://aiwhy100.com/videos/17/cover',
  coverWide: 'https://aiwhy100.com/videos/17/cover-wide',
  page: 'https://aiwhy100.com/videos/17',
};
/** 「小奇讲过啦」题库：问一样的问题时 questions/match 命中 */
const LIBRARY = ['为什么天空是蓝色的？', '为什么会打雷？', '饺子为什么会浮起来？'];

const norm = (q) => String(q ?? '').replace(/[\s?？!！。.,，]/g, '');
const newId = () => `mock${Date.now().toString(36)}${randomBytes(3).toString('hex')}`;

export function createMock({ credits = Number(env.MOCK_CREDITS ?? 20), key = env.MOCK_KEY || '', webhookUrl = env.MOCK_WEBHOOK_URL || '', webhookSecret = env.MOCK_WEBHOOK_SECRET || 'whsec_mock', queueSec = QUEUE_SEC, doneSec = DONE_SEC, log = () => {} } = {}) {
  /** @type {Map<string, any>} */
  const videos = new Map();
  const state = { credits };
  const hooked = new Set();

  // ── 视频状态按时间推进 ──
  function statusOf(v, now = Date.now()) {
    if (v.cancelled) return 'cancelled';
    const sec = (now - v.createdAt) / 1000;
    if (v.library) return 'done';
    if (sec < queueSec) return 'queued';
    if (v.fail) return 'failed';
    return sec < doneSec ? 'producing' : 'done';
  }
  const STATUS_TEXT = { queued: '思考中', producing: '制作中', done: '已完成', failed: '失败（已退还额度）', cancelled: '已取消（已退还额度）' };
  const finishedAt = (v, s) => (s === 'done' || s === 'failed' ? new Date(v.createdAt + (v.library ? 0 : (s === 'failed' ? queueSec : doneSec) * 1000)).toISOString() : null);
  const eta = (v, s, now = Date.now()) => (s === 'queued' || s === 'producing' ? Math.max(1, Math.ceil((v.createdAt + doneSec * 1000 - now) / 60000)) : null);
  const titleOf = (v) => v.question.replace(/？$/, '');

  function appView(v) {
    const s = statusOf(v);
    const done = s === 'done';
    return {
      id: v.id,
      question: v.question,
      title: done ? titleOf(v) : null,
      style: v.style,
      channel: v.channel,
      status: s,
      statusText: STATUS_TEXT[s],
      step: { queued: 0, producing: 1, done: 2 }[s] ?? null,
      queuePosition: s === 'queued' ? 1 : s === 'producing' ? 0 : null,
      etaMinutes: eta(v, s),
      progress: s === 'queued' ? 0 : s === 'producing' ? Math.min(0.99, (Date.now() - v.createdAt) / 1000 / doneSec) : null,
      summary: done ? '（模拟）这是一条示例视频，正式接口会返回这个问题的讲解摘要。' : null,
      durationSec: done ? 101 : null,
      failReason: s === 'failed' ? '这个问题我们换一个吧（模拟的制作失败）' : null,
      cancellable: s === 'queued',
      refunded: s === 'failed' || s === 'cancelled',
      demo: false,
      shared: !!v.library,
      links: done ? { video: SAMPLE.video, cover3x4: SAMPLE.cover, subtitles: null, download: SAMPLE.video, share: SAMPLE.page } : null,
      createdAt: new Date(v.createdAt).toISOString(),
      finishedAt: finishedAt(v, s),
    };
  }

  function partnerView(v) {
    const s = statusOf(v);
    const done = s === 'done';
    return {
      id: v.id,
      externalUserId: v.externalUserId ?? null,
      status: s,
      statusText: STATUS_TEXT[s],
      channel: v.channel,
      question: v.question,
      title: done ? titleOf(v) : null,
      text: done ? '（模拟）这是一条示例视频，正式接口会返回 1–2 句讲解。' : null,
      experiment: done && v.channel === 'kids' ? '（模拟）你可以试试：……' : null,
      followUpQuestion: done && v.channel === 'kids' ? '（模拟）那……又是为什么呢？' : null,
      durationSec: done ? 101 : null,
      etaMinutes: eta(v, s),
      cover: done ? { portrait3x4: SAMPLE.cover, landscape4x3: SAMPLE.coverWide, landscape16x9: SAMPLE.coverWide } : null,
      video: done ? { url: SAMPLE.video, downloadUrl: SAMPLE.video, subtitlesUrl: null } : null,
      failReason: s === 'failed' ? '这个问题我们换一个吧（模拟的制作失败）' : null,
      refunded: s === 'failed' || s === 'cancelled',
      createdAt: new Date(v.createdAt).toISOString(),
      finishedAt: finishedAt(v, s),
    };
  }

  // ── 提交时的校验（和线上一致的几种情况） ──
  function validate(question) {
    const q = String(question ?? '').trim();
    if (q.length < 4) throw err(422, '问题太短了，请说得完整一点');
    if (q.length > 120) throw err(422, '问题太长了，最多 120 个字');
    if (q.includes('拒绝')) throw err(422, '这个问题我们换一个吧，可以问问大自然里的「为什么」（模拟的拒绝）');
    return q;
  }
  function charge() {
    if (state.credits <= 0) throw err(402, '额度不足（模拟）');
    state.credits--;
  }
  function create(owner, body, kind) {
    const question = validate(body.question);
    if (kind === 'partner' && body.requestId) {
      const dup = [...videos.values()].find((v) => v.owner === owner && v.requestId === body.requestId);
      if (dup) return { v: dup, dup: true };
    }
    if (kind === 'app') {
      const unfinished = [...videos.values()].filter((v) => v.owner === owner && !v.library && ['queued', 'producing'].includes(statusOf(v))).length;
      if (unfinished >= 2) throw err(429, '已经有 2 个视频在做了，做好再问吧（模拟）');
    }
    charge();
    const v = {
      id: newId(), owner, kind, question, createdAt: Date.now(),
      channel: body.channel === 'web' ? 'web' : 'kids',
      style: body.style || '手绘卡通',
      externalUserId: body.externalUserId, requestId: body.requestId,
      fail: question.includes('失败'),
    };
    videos.set(v.id, v);
    log(`提交「${question}」→ ${v.id}`);
    return { v, dup: false };
  }
  // 失败 / 取消退还额度（只退一次）
  function refundIfNeeded(v) {
    const s = statusOf(v);
    if ((s === 'failed' || s === 'cancelled') && !v.refundedOnce) {
      v.refundedOnce = true;
      state.credits++;
    }
  }

  // ── 回调（合作厂商） ──
  async function sendHooks() {
    if (!webhookUrl) return;
    for (const v of videos.values()) {
      if (v.kind !== 'partner' || hooked.has(v.id)) continue;
      const s = statusOf(v);
      if (s !== 'done' && s !== 'failed') continue;
      hooked.add(v.id);
      const body = JSON.stringify({ event: `video.${s}`, sentAt: new Date().toISOString(), video: partnerView(v) });
      const ts = Math.floor(Date.now() / 1000);
      const sig = `v1=${createHmac('sha256', webhookSecret).update(`${ts}.${body}`).digest('hex')}`;
      fetch(webhookUrl, { method: 'POST', body, headers: { 'Content-Type': 'application/json', 'X-SVF-Event': `video.${s}`, 'X-SVF-Timestamp': String(ts), 'X-SVF-Signature': sig } })
        .then((r) => log(`回调 ${v.id} ${s} → ${r.status}`))
        .catch((e) => log(`回调失败 ${v.id}：${e.message}`));
    }
  }
  const hookTimer = setInterval(sendHooks, 1000);
  hookTimer.unref();

  // ── MCP（JSON-RPC，无状态） ──
  const TOOLS = [
    { name: 'create_why_video', description: '把孩子的「为什么」问题做成科普动画视频（模拟）', inputSchema: { type: 'object', properties: { question: { type: 'string', description: '问题，4–120 字' }, style: { type: 'string' } }, required: ['question'] } },
    { name: 'get_video', description: '查视频进度；完成时附播放链接（模拟）', inputSchema: { type: 'object', properties: { id: { type: 'string' } }, required: ['id'] } },
    { name: 'list_my_videos', description: '我的视频列表（模拟）', inputSchema: { type: 'object', properties: { limit: { type: 'number' } } } },
    { name: 'get_my_quota', description: '剩余额度（模拟）', inputSchema: { type: 'object', properties: {} } },
  ];
  function mcpCall(owner, name, args = {}) {
    const text = (o, t) => ({ content: [{ type: 'text', text: t }], structuredContent: o });
    try {
      if (name === 'create_why_video') {
        const { v } = create(owner, { question: args.question, style: args.style }, 'app');
        const o = appView(v);
        return text(o, `已提交「${o.question}」，视频 ID：${o.id}，大约 ${o.etaMinutes} 分钟后做好。`);
      }
      if (name === 'get_video') {
        const v = videos.get(String(args.id ?? ''));
        if (!v || v.owner !== owner) throw err(404, '视频不存在');
        refundIfNeeded(v);
        const o = appView(v);
        return text(o, o.status === 'done' ? `「${o.title}」做好了：${o.links.video}` : `${o.statusText}${o.etaMinutes ? `，大约还要 ${o.etaMinutes} 分钟` : ''}`);
      }
      if (name === 'list_my_videos') {
        const items = [...videos.values()].filter((v) => v.owner === owner).map(appView).slice(0, Number(args.limit) || 10);
        return text({ items }, items.map((o) => `${o.id} ${o.statusText}「${o.question}」`).join('\n') || '还没有视频');
      }
      if (name === 'get_my_quota') return text({ available: state.credits }, `剩余 ${state.credits} 条`);
      throw err(400, `没有这个工具：${name}`);
    } catch (e) {
      return { content: [{ type: 'text', text: e.message }], isError: true };
    }
  }
  function mcp(owner, msg) {
    if (msg.id === undefined) return null; // 通知不回
    const ok = (result) => ({ jsonrpc: '2.0', id: msg.id, result });
    if (msg.method === 'initialize') return ok({ protocolVersion: msg.params?.protocolVersion ?? '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'aiwhy100-mock', version: '1.0.0' } });
    if (msg.method === 'ping') return ok({});
    if (msg.method === 'tools/list') return ok({ tools: TOOLS });
    if (msg.method === 'tools/call') return ok(mcpCall(owner, msg.params?.name, msg.params?.arguments));
    return { jsonrpc: '2.0', id: msg.id, error: { code: -32601, message: `Method not found: ${msg.method}` } };
  }

  // ── HTTP ──
  async function route(req, res) {
    const url = new URL(req.url, 'http://x');
    const p = url.pathname.replace(/\/+$/, '') || '/';
    const send = (code, obj) => {
      res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify(obj));
    };
    if (p === '/' || p === '/health') return send(200, { ok: true, mock: true, credits: state.credits, videos: videos.size });

    const auth = String(req.headers.authorization ?? '');
    const token = auth.replace(/^Bearer\s+/i, '').trim();
    const authed = () => {
      if (!token || (key && token !== key)) throw err(401, 'API Key 无效（模拟服务器：带任意 Bearer 令牌即可；设了 MOCK_KEY 时要一致）');
      return `key:${token}`;
    };

    // 个人开发者接口
    if (p.startsWith('/api/app/v1/')) {
      const sub = p.slice('/api/app/v1'.length);
      if (sub === '/config' && req.method === 'GET') return send(200, { asr: true });
      const owner = authed();
      if (sub === '/asr' && req.method === 'POST') {
        const body = await readBody(req, 1024 * 1024);
        const format = url.searchParams.get('format') ?? 'opus';
        if (!['pcm16', 'opus'].includes(format)) throw err(400, 'format 只能是 opus 或 pcm16');
        if (!body.length) throw err(400, '请求体应为录音数据');
        // 模拟识别：pcm16 按长度算时长，太短当作没听清；识别结果用请求头 X-Mock-Question 或 MOCK_QUESTION 指定
        const audioMs = format === 'pcm16' ? Math.round(body.length / 32) : 2000;
        const text = audioMs < 300 ? '' : decodeURIComponent(String(req.headers['x-mock-question'] ?? '')) || env.MOCK_QUESTION || '为什么天空是蓝色的';
        const question = text ? `${text.replace(/[？?]$/, '')}？` : '';
        log(`识别 ${(audioMs / 1000).toFixed(1)} 秒 →「${question}」`);
        return send(200, { text: text.replace(/[？?]$/, ''), question, audioMs: Math.min(audioMs, 15000), truncated: audioMs > 15000 });
      }
      if (sub === '/me' && req.method === 'GET') return send(200, { user: { id: 'mock-user', email: 'dev@example.com' }, credits: { available: state.credits } });
      if (sub === '/questions/match' && req.method === 'POST') {
        const b = await readJson(req);
        const hit = LIBRARY.find((q) => norm(q) === norm(b.question));
        if (!hit) return send(200, { match: null });
        let v = [...videos.values()].find((x) => x.library && x.question === hit);
        if (!v) {
          v = { id: newId(), owner: 'library', kind: 'app', question: hit, createdAt: Date.now() - 86400_000, channel: 'kids', style: '手绘卡通', library: true };
          videos.set(v.id, v);
        }
        return send(200, { match: { own: false, video: appView(v) } });
      }
      let m = sub.match(/^\/videos\/([^/]+)\/save$/);
      if (m && req.method === 'POST') {
        const v = videos.get(m[1]);
        if (!v?.library) throw err(404, '视频不存在');
        (v.savedBy ??= new Set()).add(owner);
        return send(200, appView(v));
      }
      if (sub === '/videos' && req.method === 'POST') {
        const { v } = create(owner, await readJson(req), 'app');
        return send(200, appView(v));
      }
      if (sub === '/videos' && req.method === 'GET') {
        const items = [...videos.values()].filter((v) => v.owner === owner || v.savedBy?.has(owner)).sort((a, b) => b.createdAt - a.createdAt).map((v) => (refundIfNeeded(v), appView(v)));
        return send(200, { items, total: items.length });
      }
      m = sub.match(/^\/videos\/([^/]+)$/);
      if (m) {
        const v = videos.get(m[1]);
        if (!v || (v.owner !== owner && !v.library)) throw err(404, '视频不存在');
        if (req.method === 'DELETE') {
          if (statusOf(v) !== 'queued') throw err(409, '已经开始制作，不能取消了');
          v.cancelled = true;
        }
        refundIfNeeded(v);
        return send(200, appView(v));
      }
      throw err(404, '没有这个接口');
    }

    // 合作厂商接口
    if (p.startsWith('/api/partner/v1/')) {
      const owner = authed();
      const sub = p.slice('/api/partner/v1'.length);
      if (sub === '/account' && req.method === 'GET') return send(200, { name: '模拟厂商', channel: 'kids', credits: state.credits, unfinished: [...videos.values()].filter((v) => v.owner === owner && ['queued', 'producing'].includes(statusOf(v))).length, webhook: !!webhookUrl, ipAllowlist: [], clientIp: req.socket.remoteAddress });
      if (sub === '/videos' && req.method === 'POST') {
        const b = await readJson(req);
        if (req.headers['idempotency-key'] && !b.requestId) b.requestId = String(req.headers['idempotency-key']);
        const { v, dup } = create(owner, b, 'partner');
        return send(dup ? 200 : 201, partnerView(v));
      }
      if (sub === '/videos' && req.method === 'GET') {
        const ext = url.searchParams.get('externalUserId');
        const items = [...videos.values()].filter((v) => v.owner === owner && (!ext || v.externalUserId === ext)).sort((a, b) => b.createdAt - a.createdAt).map(partnerView);
        return send(200, { items, total: items.length, page: 1, pageSize: items.length });
      }
      const m = sub.match(/^\/videos\/([^/]+)$/);
      if (m) {
        const v = videos.get(m[1]);
        if (!v || v.owner !== owner) throw err(404, '视频不存在');
        if (req.method === 'DELETE') {
          if (statusOf(v) !== 'queued') throw err(409, '已经开始制作，不能取消了');
          v.cancelled = true;
        }
        refundIfNeeded(v);
        return send(200, partnerView(v));
      }
      throw err(404, '没有这个接口');
    }

    // MCP
    if (p === '/mcp') {
      if (req.method !== 'POST') {
        res.writeHead(405, { Allow: 'POST' });
        return res.end();
      }
      const owner = authed();
      const msg = await readJson(req);
      const out = Array.isArray(msg) ? msg.map((x) => mcp(owner, x)).filter(Boolean) : mcp(owner, msg);
      if (!out || (Array.isArray(out) && !out.length)) {
        res.writeHead(202);
        return res.end();
      }
      return send(200, out);
    }
    throw err(404, '没有这个接口');
  }

  const server = http.createServer((req, res) => {
    route(req, res).catch((e) => {
      const statusCode = e.statusCode ?? 500;
      if (statusCode === 500) log(`出错 ${req.method} ${req.url}：${e.message}`);
      if (!res.headersSent) {
        res.writeHead(statusCode, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ statusCode, error: 'HttpError', message: e.message }));
      }
    });
  });
  return { server, state, videos, close: () => (clearInterval(hookTimer), new Promise((r) => server.close(r))) };
}

function err(statusCode, message) {
  return Object.assign(new Error(message), { statusCode });
}
function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(err(413, '请求太大'));
        req.destroy();
      } else chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}
async function readJson(req) {
  const b = await readBody(req, 64 * 1024);
  try {
    return b.length ? JSON.parse(b.toString('utf8')) : {};
  } catch {
    throw err(400, '请求体不是 JSON');
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const port = Number(env.PORT ?? 8787);
  const ts = () => new Date().toLocaleTimeString('zh-CN', { hour12: false });
  const { server } = createMock({ log: (m) => console.log(ts(), m) });
  server.listen(port, env.HOST ?? '0.0.0.0', () => {
    console.log(`${ts()} 模拟服务器已启动：http://127.0.0.1:${port}（视频 ${DONE_SEC} 秒做好；MOCK_FAST=1 改成 3 秒）`);
    console.log(`${ts()} 设备 / 示例里把接口地址换成 http://<本机 IP>:${port}，API Key 随便填一个`);
  });
}
