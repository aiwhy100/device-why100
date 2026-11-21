// 小智桥接：用一个本地 WebSocket 服务冒充小智接入点，MCP 用模拟服务器
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { WebSocketServer } from 'ws';
import { startBridge } from '../integrations/xiaozhi/bridge.mjs';
import { createMock } from '../mock/server.mjs';

describe('小智 MCP 桥接', { skip: !globalThis.WebSocket && '需要 Node.js 22+' }, () => {
  let mock, wss, bridge, peer;
  const inbox = [];
  const waiters = [];
  const next = (id) =>
    new Promise((resolve) => {
      const look = () => {
        const i = inbox.findIndex((m) => m.id === id);
        if (i >= 0) return resolve(inbox.splice(i, 1)[0]), true;
        return false;
      };
      if (!look()) waiters.push(look);
    });

  before(async () => {
    mock = createMock({ queueSec: 0.1, doneSec: 0.3 });
    await new Promise((r) => mock.server.listen(0, '127.0.0.1', r));
    wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.on('listening', r));
    const connected = new Promise((r) => wss.on('connection', (s) => r(s)));
    bridge = startBridge({
      endpoint: `ws://127.0.0.1:${wss.address().port}/mcp/?token=x`,
      mcpUrl: `http://127.0.0.1:${mock.server.address().port}/mcp`,
      apiKey: 'svf_live_test',
      log: () => {},
    });
    peer = await connected;
    peer.on('message', (d) => {
      inbox.push(JSON.parse(d.toString()));
      for (const w of waiters.splice(0)) if (!w()) waiters.push(w);
    });
  });
  after(async () => {
    bridge.stop();
    wss.close();
    await mock.close();
  });

  const rpc = (id, method, params) => peer.send(JSON.stringify({ jsonrpc: '2.0', id, method, params }));

  it('initialize → tools/list → tools/call create_why_video → get_video', async () => {
    rpc(1, 'initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'xiaozhi', version: '1' } });
    assert.equal((await next(1)).result.serverInfo.name, 'aiwhy100-mock');
    peer.send(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }));
    rpc(2, 'tools/list', {});
    assert.ok((await next(2)).result.tools.some((t) => t.name === 'create_why_video'));
    rpc(3, 'tools/call', { name: 'create_why_video', arguments: { question: '为什么天空是蓝色的？' } });
    const created = (await next(3)).result;
    assert.equal(created.isError, undefined);
    const id = created.structuredContent.id;
    await new Promise((r) => setTimeout(r, 400));
    rpc(4, 'tools/call', { name: 'get_video', arguments: { id } });
    assert.match((await next(4)).result.content[0].text, /做好了/);
  });

  it('问题不合适：工具返回 isError，文字可以直接读给孩子', async () => {
    rpc(5, 'tools/call', { name: 'create_why_video', arguments: { question: '我要拒绝这个问题' } });
    const r = (await next(5)).result;
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /换一个/);
  });
});
