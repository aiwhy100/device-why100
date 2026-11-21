#!/usr/bin/env node
// 自建网关：设备只和网关通信，API Key 只放在网关这边。
//   设备：POST 录音 → 网关识别（ASR_URL）→ 查「小奇讲过啦」/ 提交 AI为什么100 → 后台轮询（或收回调）→ 设备来问时告诉它结果
//   家长：手机打开 http://<网关地址>/ 看视频
// 设备接口见 docs/gateway.md「设备接口」。零依赖，Node.js 20+。
import { createHmac, randomUUID, timingSafeEqual } from 'node:crypto';
import http from 'node:http';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { loadConfig } from './config.js';
import { devicesPage, listPage, notFoundPage, videoPage } from './pages.js';
import { createStore } from './store.js';
import { createUpstream, normalize, recognize, UpstreamError } from './upstream.js';

const MAX_AUDIO = 2 * 1024 * 1024; // 16kHz 16 位单声道约 60 秒（识别只用前 15 秒）
const TERMINAL = new Set(['done', 'failed', 'cancelled']);
const DEVICE_RE = /^[0-9A-Za-z.:_@-]{1,64}$/;

export function createGateway(cfg = loadConfig(), { log = () => {} } = {}) {
  const store = createStore(cfg.dataDir);
  const up = createUpstream(cfg);

  // ---------- 提交问题 ----------
  /** 识别 + 提交，返回设备要播哪句提示语 */
  async function handleAsk(device, { pcm, text }) {
    let question = text ? String(text).trim() : '';
    if (pcm) {
      const t = performance.now();
      try {
        question = await recognize(cfg, pcm);
      } catch (e) {
        log(`识别失败 ${device}：${e.status ?? ''} ${e.message}`);
        return { prompt: 'net_error' };
      }
      log(`识别 ${device} ${(pcm.length / 32000).toFixed(1)} 秒 → ${(performance.now() - t).toFixed(0)} ms「${question}」`);
    }
    // 录音只在内存里，函数返回后就丢掉
    if (question && !/[?？]$/.test(question)) question += '？';
    if (question.replace(/[？?]/g, '').length < 3) return { prompt: 'not_heard' };

    try {
      const found = await up.match(question);
      if (found) {
        store.add({ requestId: newRequestId(device), device, question, videoId: found.id, status: found.status, video: found, found: true, notified: true, createdAt: new Date().toISOString() });
        log(`小奇讲过啦 ${device}「${question}」→ ${found.id}`);
        return { prompt: 'found', question, title: found.title };
      }
    } catch (e) {
      log(`查重失败（照常提交）${device}：${e.status ?? ''} ${e.message}`);
    }

    const requestId = newRequestId(device);
    let v;
    try {
      // 网络抖动时用同一个 requestId 重试一次，合作厂商接口不会重复扣额度
      try {
        v = await up.create({ question, deviceId: device, requestId });
      } catch (e) {
        if (e instanceof UpstreamError || cfg.upstream !== 'partner') throw e;
        v = await up.create({ question, deviceId: device, requestId });
      }
    } catch (e) {
      const status = e.status;
      log(`提交失败 ${device}「${question}」：${status ?? ''} ${e.message}`);
      if (status === 400 || status === 422) return { prompt: 'rejected', question, message: e.message };
      if (status === 402) return { prompt: 'no_credit', question, message: e.message };
      if (status === 429) return { prompt: 'busy', question, message: e.message };
      if (status === 401 || status === 403) return { prompt: 'bad_key', question, message: e.message };
      return { prompt: 'net_error', question };
    }
    store.add({ requestId, device, question, videoId: v.id, status: v.status, video: v, notified: false, createdAt: new Date().toISOString(), nextPollAt: Date.now() + 15_000 });
    log(`提交 ${device}「${question}」→ ${v.id} ${v.statusText ?? v.status}，预计 ${v.etaMinutes ?? '?'} 分钟`);
    return { prompt: 'got', question, etaMinutes: v.etaMinutes ?? null };
  }

  function update(a, v) {
    if (v.status !== a.status) log(`状态 ${a.device}「${a.question}」${a.status} → ${v.status}${v.title ? `「${v.title}」` : ''}`);
    a.status = v.status;
    a.video = v;
  }

  // ---------- 后台轮询（设了回调也照样轮询，频率低一些，防止漏掉） ----------
  let polling = false;
  async function pollOnce() {
    if (polling) return;
    polling = true;
    try {
      const now = Date.now();
      for (const a of store.all()) {
        if (TERMINAL.has(a.status) || !a.videoId || (a.nextPollAt ?? 0) > now) continue;
        try {
          update(a, await up.get(a.videoId));
          // 思考中每 15 秒、制作中每 30 秒查一次；有回调时放慢到 2 分钟
          a.nextPollAt = Date.now() + (cfg.webhookSecret ? 120_000 : a.status === 'queued' ? 15_000 : 30_000);
        } catch (e) {
          log(`查询失败 ${a.videoId}：${e.status ?? ''} ${e.message}`);
          a.nextPollAt = Date.now() + 60_000;
        }
      }
      store.save();
    } finally {
      polling = false;
    }
  }

  // 封面 / 视频的固定地址：打开时换成最新的播放链接（个人开发者接口的链接 1 小时过期），10 分钟内复用
  async function freshLinks(a) {
    if (!a.videoId || a.status !== 'done') return a.video;
    if (Date.now() - (a.refreshedAt ?? 0) > 10 * 60_000) {
      try {
        update(a, await up.get(a.videoId));
        a.refreshedAt = Date.now();
        store.save();
      } catch {}
    }
    return a.video;
  }

  // ---------- HTTP ----------
  const json = (res, code, obj) => {
    res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(obj));
  };
  const html = (res, code, body) => {
    res.writeHead(code, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(body);
  };
  const redirect = (res, url) => {
    if (!url) return html(res, 404, notFoundPage());
    res.writeHead(302, { Location: url, 'Cache-Control': 'no-store' });
    res.end();
  };
  const safeEq = (a, b) => {
    const x = Buffer.from(String(a));
    const y = Buffer.from(String(b));
    return x.length === y.length && timingSafeEqual(x, y);
  };

  async function route(req, res) {
    const url = new URL(req.url, 'http://x');
    const parts = url.pathname.split('/').filter(Boolean).map(decodeURIComponent);

    if (parts[0] === 'healthz') return json(res, 200, { ok: true, upstream: cfg.upstream, asr: !!cfg.asrUrl, key: !!cfg.apiKey });

    // 设备接口：/api/device/<设备ID>/{ask,ask-text,poll,ack}
    if (parts[0] === 'api' && parts[1] === 'device') {
      const device = (parts[2] ?? '').toLowerCase();
      if (!DEVICE_RE.test(device)) return json(res, 400, { error: 'bad device id' });
      if (cfg.deviceTokens.length) {
        const t = String(req.headers['x-device-token'] ?? '');
        if (!cfg.deviceTokens.some((d) => safeEq(d, t))) return json(res, 401, { error: 'bad device token' });
      }
      const action = parts[3];
      if (req.method === 'POST' && action === 'ask') {
        // 正文：16kHz 单声道 16 位小端 PCM
        return json(res, 200, await handleAsk(device, { pcm: await readBody(req, MAX_AUDIO) }));
      }
      if (req.method === 'POST' && action === 'ask-text') {
        const { question } = parseJson(await readBody(req, 4096));
        return json(res, 200, await handleAsk(device, { text: String(question ?? '') }));
      }
      if (req.method === 'GET' && action === 'poll') {
        const mine = store.byDevice(device).filter((a) => !a.notified);
        const ready = mine.filter((a) => a.status === 'done');
        const failed = mine.filter((a) => a.status === 'failed');
        return json(res, 200, {
          ready: ready.length, ids: ready.map((a) => a.requestId),
          failed: failed.length, failedIds: failed.map((a) => a.requestId),
          pending: store.byDevice(device).filter((a) => a.status === 'queued' || a.status === 'producing').length,
        });
      }
      if (req.method === 'POST' && action === 'ack') {
        const { ids = [] } = parseJson(await readBody(req, 8192));
        for (const id of Array.isArray(ids) ? ids : []) {
          const a = store.find(String(id));
          if (a && a.device === device) a.notified = true;
        }
        store.save();
        return json(res, 200, { ok: true });
      }
      return json(res, 404, { error: 'not found' });
    }

    // 回调（合作厂商在后台「接入设置」里填 https://<网关>/webhook/aiwhy100）
    if (parts[0] === 'webhook' && parts[1] === 'aiwhy100' && req.method === 'POST') {
      if (!cfg.webhookSecret) return json(res, 404, { error: 'webhook not enabled' });
      const raw = await readBody(req, 256 * 1024);
      const ts = String(req.headers['x-svf-timestamp'] ?? '');
      const sig = String(req.headers['x-svf-signature'] ?? '');
      const expect = `v1=${createHmac('sha256', cfg.webhookSecret).update(`${ts}.`).update(raw).digest('hex')}`;
      if (Math.abs(Date.now() / 1000 - Number(ts)) > 300 || !safeEq(expect, sig)) return json(res, 401, { error: 'bad signature' });
      const body = parseJson(raw);
      if (body.event === 'ping') return json(res, 200, { ok: true });
      const a = body.video?.id && store.findByVideo(body.video.id);
      if (a) {
        update(a, normalize(body.video));
        store.save();
      }
      return json(res, 200, { ok: true });
    }

    // 家长网页（设了 PARENT_PASSWORD 时要输入密码，用户名随意）
    if (req.method !== 'GET') return json(res, 405, { error: 'method not allowed' });
    if (cfg.parentPassword) {
      const [, b64 = ''] = String(req.headers.authorization ?? '').split(' ');
      const pass = Buffer.from(b64, 'base64').toString().split(':').slice(1).join(':');
      if (!safeEq(pass, cfg.parentPassword)) {
        res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="aiwhy100", charset="UTF-8"' });
        return res.end('need password');
      }
    }
    if (parts.length === 0) {
      const devs = store.devices();
      if (devs.length === 1) return redirect(res, `/d/${encodeURIComponent(devs[0])}`);
      return html(res, 200, devicesPage(devs.map((device) => ({ device, asks: store.byDevice(device) }))));
    }
    if (parts[0] === 'd' && parts[1]) return html(res, 200, listPage(parts[1].toLowerCase(), store.byDevice(parts[1].toLowerCase())));
    const a = parts[1] ? store.find(parts[1]) : null;
    if (parts[0] === 'v') return a ? html(res, 200, videoPage({ ...a, video: await freshLinks(a) })) : html(res, 404, notFoundPage());
    if (parts[0] === 'c') return redirect(res, a && (await freshLinks(a))?.coverUrl);
    if (parts[0] === 'm') return redirect(res, a && (await freshLinks(a))?.videoUrl);
    return html(res, 404, notFoundPage());
  }

  const server = http.createServer((req, res) => {
    route(req, res).catch((e) => {
      log(`出错 ${req.method} ${req.url.split('?')[0]}：${e.message}`);
      if (!res.headersSent) json(res, e.code === 413 ? 413 : e.code === 400 ? 400 : 500, { error: e.message });
    });
  });
  const timer = setInterval(pollOnce, 5_000);
  timer.unref();
  return { server, store, pollOnce, close: () => (clearInterval(timer), new Promise((r) => server.close(r))) };
}

const newRequestId = (device) => `gw-${device.replace(/[^0-9a-z]/gi, '').slice(-12)}-${Date.now().toString(36)}-${randomUUID().slice(0, 6)}`;

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(Object.assign(new Error('请求太大'), { code: 413 }));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}
function parseJson(buf) {
  try {
    return buf.length ? JSON.parse(buf.toString('utf8')) : {};
  } catch {
    throw Object.assign(new Error('请求体不是 JSON'), { code: 400 });
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const cfg = loadConfig();
  const ts = () => new Date().toLocaleTimeString('zh-CN', { hour12: false });
  const log = (...m) => console.log(ts(), ...m);
  if (!cfg.apiKey) log('⚠️  没有设置 AIWHY100_API_KEY：设备提问会失败（个人开发者在 https://aiwhy100.com/app/ 「设置 → API Key」创建）');
  if (!cfg.asrUrl) log('⚠️  没有设置 ASR_URL：设备录音没法识别（合作厂商请部署 asr-server，见 docs/gateway.md）');
  if (!cfg.deviceTokens.length) log('提示：没有设置 DEVICE_TOKENS，局域网里任何设备都能提问；放到公网上一定要设');
  const { server } = createGateway(cfg, { log });
  server.listen(cfg.port, '0.0.0.0', () => {
    log(`网关已启动：端口 ${cfg.port}，上游 ${cfg.upstream === 'partner' ? '合作厂商接口' : '个人开发者接口'} ${cfg.apiBase}，识别 ${cfg.asrUrl || '（未配置）'}`);
    const ips = Object.values(os.networkInterfaces()).flat().filter((i) => i && i.family === 'IPv4' && !i.internal).map((i) => i.address);
    for (const ip of ips) log(`  家长网页 / 设备地址：http://${ip}:${cfg.port}/`);
  });
}
