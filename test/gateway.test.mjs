// 网关 + 模拟服务器的端到端测试：node --test test/
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { createGateway } from '../gateway/src/server.js';
import { loadConfig } from '../gateway/src/config.js';
import { createMock } from '../mock/server.mjs';

const listen = (server) => new Promise((r) => server.listen(0, '127.0.0.1', () => r(`http://127.0.0.1:${server.address().port}`)));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pcm = (sec) => Buffer.alloc(Math.round(sec * 16000) * 2, 1);
const DEV = '24:0a:c4:12:34:56';

async function waitFor(fn, ms = 8000) {
  const t = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() - t > ms) throw new Error('等待超时');
    await sleep(100);
  }
}

describe('网关：个人开发者接口（UPSTREAM=app）', () => {
  let mock, gw, base, dir;
  before(async () => {
    mock = createMock({ credits: 3, queueSec: 0.3, doneSec: 0.8 });
    const mockUrl = await listen(mock.server);
    dir = mkdtempSync(join(tmpdir(), 'gw-'));
    const cfg = loadConfig({ AIWHY100_BASE_URL: mockUrl, AIWHY100_API_KEY: 'svf_live_test', DATA_DIR: dir, DEVICE_TOKENS: 'dev-secret' });
    assert.equal(cfg.asrUrl, `${mockUrl}/api/app/v1/asr`, '个人开发者默认用线上识别');
    assert.equal(cfg.asrToken, 'svf_live_test');
    gw = createGateway(cfg);
    base = await listen(gw.server);
  });
  after(async () => {
    await gw.close();
    await mock.close();
    rmSync(dir, { recursive: true, force: true });
  });

  const dev = (action, body, ctype = 'application/json', token = 'dev-secret') =>
    fetch(`${base}/api/device/${DEV}/${action}`, {
      method: body === undefined ? 'GET' : 'POST',
      body: body === undefined ? undefined : Buffer.isBuffer(body) ? body : JSON.stringify(body),
      headers: { 'Content-Type': ctype, 'X-Device-Token': token },
    });

  it('设备令牌不对：401', async () => {
    assert.equal((await dev('poll', undefined, 'application/json', 'nope')).status, 401);
  });

  it('录音 → 识别 → 提交', async () => {
    process.env.MOCK_QUESTION = '为什么月亮会跟着我走'; // 模拟服务器的识别结果
    const r = await (await dev('ask', pcm(2), 'application/octet-stream')).json();
    delete process.env.MOCK_QUESTION;
    assert.deepEqual(r, { prompt: 'got', question: '为什么月亮会跟着我走？', etaMinutes: 1 });
  });

  it('录音识别出题库里的问题：走查重，小奇讲过啦', async () => {
    const r = await (await dev('ask', pcm(2), 'application/octet-stream')).json();
    assert.equal(r.prompt, 'found');
    assert.equal(r.question, '为什么天空是蓝色的？');
  });

  it('题库里有的问题：小奇讲过啦，不扣额度', async () => {
    const before = mock.state.credits;
    const r = await (await dev('ask-text', { question: '为什么会打雷' })).json();
    assert.equal(r.prompt, 'found');
    assert.equal(mock.state.credits, before);
  });

  it('新问题：提交、轮询到完成，poll 报 ready，ack 后清掉；家长网页能看', async () => {
    const r = await (await dev('ask-text', { question: '为什么猫咪会呼噜' })).json();
    assert.equal(r.prompt, 'got');
    await gw.pollOnce();
    const ready = await waitFor(async () => {
      for (const a of gw.store.all()) a.nextPollAt = 0;
      await gw.pollOnce();
      const p = await (await dev('poll')).json();
      return p.ready >= 1 && p;
    });
    assert.ok(ready.ids.length >= 1);
    await dev('ack', { ids: ready.ids });
    const after = await (await dev('poll')).json();
    assert.equal(after.ready, 0);

    const list = await fetch(`${base}/d/${encodeURIComponent(DEV)}`);
    const page = await list.text();
    assert.match(page, /为什么猫咪会呼噜/);
    const v = await fetch(`${base}/m/${ready.ids[0]}`, { redirect: 'manual' });
    assert.equal(v.status, 302);
    assert.match(v.headers.get('location'), /aiwhy100\.com\/videos\/17\/video/);
  });

  it('各种失败映射到设备提示语', async () => {
    assert.equal((await (await dev('ask-text', { question: '我要拒绝这个' })).json()).prompt, 'rejected');
    assert.equal((await (await dev('ask-text', { question: '嗯' })).json()).prompt, 'not_heard');
    assert.equal((await (await dev('ask', pcm(0.1), 'application/octet-stream')).json()).prompt, 'not_heard');
    // 额度用完：402
    mock.state.credits = 0;
    assert.equal((await (await dev('ask-text', { question: '为什么海水是咸的' })).json()).prompt, 'no_credit');
  });

  it('制作失败：poll 报 failed', async () => {
    mock.state.credits = 5;
    for (const v of mock.videos.values()) v.cancelled = true; // 清掉在做的，避开「同时 2 个」限制
    assert.equal((await (await dev('ask-text', { question: '为什么会失败呢' })).json()).prompt, 'got');
    const p = await waitFor(async () => {
      for (const a of gw.store.all()) a.nextPollAt = 0;
      await gw.pollOnce();
      const p = await (await dev('poll')).json();
      return p.failed >= 1 && p;
    });
    assert.equal(p.failedIds.length, 1);
  });
});

describe('网关：合作厂商接口（UPSTREAM=partner）+ 回调', () => {
  let mock, gw, base, dir;
  before(async () => {
    dir = mkdtempSync(join(tmpdir(), 'gw-'));
    // 先起网关拿到地址，再起带回调的模拟服务器
    const placeholder = createMock();
    const mockUrl = await listen(placeholder.server);
    await placeholder.close();
    const cfg = loadConfig({
      UPSTREAM: 'partner', AIWHY100_BASE_URL: mockUrl, AIWHY100_API_KEY: 'svf_live_partner', DATA_DIR: dir,
      ASR_URL: `${mockUrl}/api/app/v1/asr`, ASR_TOKEN: 'asr-token', WEBHOOK_SECRET: 'whsec_test',
    });
    gw = createGateway(cfg);
    base = await listen(gw.server);
    mock = createMock({ queueSec: 0.2, doneSec: 0.5, webhookUrl: `${base}/webhook/aiwhy100`, webhookSecret: 'whsec_test' });
    await new Promise((r) => mock.server.listen(Number(new URL(mockUrl).port), '127.0.0.1', r));
  });
  after(async () => {
    await gw.close();
    await mock.close();
    rmSync(dir, { recursive: true, force: true });
  });

  it('提交带 externalUserId；回调签名正确时更新状态', async () => {
    const r = await (await fetch(`${base}/api/device/${DEV}/ask-text`, { method: 'POST', body: JSON.stringify({ question: '为什么天空是蓝色的' }) })).json();
    assert.equal(r.prompt, 'got', '合作厂商接口没有查重，直接提交');
    const v = [...mock.videos.values()][0];
    assert.equal(v.externalUserId, DEV);
    assert.match(v.requestId, /^gw-/);
    // 不轮询，只靠回调
    const p = await waitFor(async () => {
      const p = await (await fetch(`${base}/api/device/${DEV}/poll`)).json();
      return p.ready >= 1 && p;
    });
    assert.equal(p.ready, 1);
  });

  it('回调签名不对：401', async () => {
    const r = await fetch(`${base}/webhook/aiwhy100`, { method: 'POST', body: '{}', headers: { 'X-SVF-Timestamp': String(Math.floor(Date.now() / 1000)), 'X-SVF-Signature': 'v1=00' } });
    assert.equal(r.status, 401);
  });
});
