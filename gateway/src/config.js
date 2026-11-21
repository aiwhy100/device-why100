// 网关配置：全部来自环境变量（docker compose 读 .env，见 .env.example）。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const list = (v) => (v ?? '').split(',').map((s) => s.trim()).filter(Boolean);

export function loadConfig(env = process.env) {
  const base = (env.AIWHY100_BASE_URL || 'https://api.aiwhy100.com').replace(/\/+$/, '');
  const upstream = env.UPSTREAM === 'partner' ? 'partner' : 'app';
  // Key 优先读文件（Docker secrets / 本机 600 权限文件），其次环境变量；不打印、不写日志
  let apiKey = env.AIWHY100_API_KEY || '';
  if (env.AIWHY100_API_KEY_FILE) apiKey = fs.readFileSync(env.AIWHY100_API_KEY_FILE, 'utf8').trim();
  // 识别：个人开发者默认直接用 AI为什么100 的识别接口（同一个 Key）；合作厂商用自己部署的 asr-server
  const asrUrl = env.ASR_URL || (upstream === 'app' ? `${base}/api/app/v1/asr` : '');
  const asrToken = env.ASR_TOKEN ?? (asrUrl.startsWith(base) ? apiKey : '');
  return {
    port: Number(env.PORT || 8080),
    upstream,
    base,
    apiBase: `${base}/api/${upstream === 'partner' ? 'partner' : 'app'}/v1`,
    apiKey,
    channel: env.CHANNEL === 'web' ? 'web' : 'kids',
    asrUrl,
    asrToken,
    deviceTokens: list(env.DEVICE_TOKENS),
    parentPassword: env.PARENT_PASSWORD || '',
    webhookSecret: env.WEBHOOK_SECRET || '',
    dataDir: env.DATA_DIR || path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'data'),
    /** 「小奇讲过啦」：同一个问题已经有成片时直接收进账号，不扣额度（只有个人开发者接口有） */
    match: env.MATCH !== 'false',
  };
}
