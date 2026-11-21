// 调 AI为什么100：个人开发者接口（/api/app/v1）或合作厂商接口（/api/partner/v1），统一成同一种视频对象。
// Key 只放在网关这边，不下发到设备。

export class UpstreamError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

/** 两种接口的视频对象 → 网关内部用的统一格式 */
export function normalize(v) {
  if (!v) return null;
  const links = v.links ?? {};
  return {
    id: v.id,
    status: v.status,
    statusText: v.statusText,
    title: v.title ?? null,
    text: v.text ?? v.summary ?? null,
    experiment: v.experiment ?? null,
    followUpQuestion: v.followUpQuestion ?? null,
    videoUrl: v.video?.url ?? links.video ?? null,
    coverUrl: v.cover?.portrait3x4 ?? links.cover3x4 ?? null,
    etaMinutes: v.etaMinutes ?? null,
    failReason: v.failReason ?? null,
  };
}

export function createUpstream(cfg) {
  async function call(method, p, body) {
    if (!cfg.apiKey) throw new UpstreamError(401, '网关没有配置 AIWHY100_API_KEY');
    const res = await fetch(cfg.apiBase + p, {
      method,
      headers: { Authorization: `Bearer ${cfg.apiKey}`, ...(body ? { 'Content-Type': 'application/json' } : {}) },
      body: body ? JSON.stringify(body) : undefined,
      signal: AbortSignal.timeout(20_000),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new UpstreamError(res.status, data.message ?? `HTTP ${res.status}`);
    return data;
  }

  return {
    /** 查「小奇讲过啦」；命中就收进账号（不扣额度），返回视频，否则 null。合作厂商接口没有这个功能 */
    async match(question) {
      if (cfg.upstream !== 'app' || !cfg.match) return null;
      const m = (await call('POST', '/questions/match', { question })).match;
      if (!m) return null;
      return normalize(m.own ? m.video : await call('POST', `/videos/${encodeURIComponent(m.video.id)}/save`));
    },
    /** 提交问题。requestId 相同的重复提交只生成一条（合作厂商接口） */
    async create({ question, deviceId, requestId }) {
      const body = cfg.upstream === 'partner'
        ? { question, externalUserId: deviceId, requestId, channel: cfg.channel }
        : { question, channel: cfg.channel };
      return normalize(await call('POST', '/videos', body));
    },
    async get(id) {
      return normalize(await call('GET', `/videos/${encodeURIComponent(id)}`));
    },
  };
}

/** 录音 → 问句（POST {ASR_URL}?format=pcm16）；没听清返回空串 */
export async function recognize(cfg, pcm) {
  if (!cfg.asrUrl) throw new UpstreamError(503, '网关没有配置 ASR_URL（合作厂商请部署 asr-server）');
  const url = `${cfg.asrUrl}${cfg.asrUrl.includes('?') ? '&' : '?'}format=pcm16`;
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream', ...(cfg.asrToken ? { Authorization: `Bearer ${cfg.asrToken}` } : {}) },
    body: pcm,
    signal: AbortSignal.timeout(20_000),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new UpstreamError(res.status, data.message ?? `识别失败 HTTP ${res.status}`);
  return String(data.question ?? '');
}
