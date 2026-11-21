// 示例对着模拟服务器跑一遍：Python（只用标准库）和 curl 快速上手脚本
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createMock } from '../mock/server.mjs';

const run = promisify(execFile);
const root = fileURLToPath(new URL('..', import.meta.url));
const has = async (cmd) => run('sh', ['-c', `command -v ${cmd}`]).then(() => true, () => false);

describe('示例', () => {
  let mock, base;
  before(async () => {
    mock = createMock({ queueSec: 0.2, doneSec: 0.6 });
    await new Promise((r) => mock.server.listen(0, '127.0.0.1', r));
    base = `http://127.0.0.1:${mock.server.address().port}`;
  });
  after(() => mock.close());
  const env = () => ({ ...process.env, AIWHY100_BASE_URL: base, AIWHY100_API_KEY: 'svf_live_test' });

  it('python why.py：提问并等到做好', async (t) => {
    if (!(await has('python3'))) return t.skip('没有 python3');
    const { stdout } = await run('python3', ['examples/python/why.py', '为什么月亮会跟着我走', '--every', '1'], { cwd: root, env: env() });
    assert.match(stdout, /已提交/);
    assert.match(stdout, /做好啦/);
  });

  it('python why.py：题库里的问题走「小奇讲过啦」', async (t) => {
    if (!(await has('python3'))) return t.skip('没有 python3');
    const { stdout } = await run('python3', ['examples/python/why.py', '为什么会打雷？'], { cwd: root, env: env() });
    assert.match(stdout, /小奇讲过啦/);
  });

  it('curl quickstart.sh', async (t) => {
    if (!(await has('curl'))) return t.skip('没有 curl');
    const { stdout } = await run('sh', ['examples/curl/quickstart.sh', '为什么猫咪会呼噜'], { cwd: root, env: { ...env(), POLL_SECONDS: '1' } });
    assert.match(stdout, /"status":"done"/);
  });
});
