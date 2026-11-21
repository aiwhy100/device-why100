// 家长手机网页（服务端直接拼 HTML，无前端框架）。
// 封面和视频都走网关的固定地址 /c/<编号>、/m/<编号>，打开时再换成最新的播放链接（个人开发者接口的链接 1 小时过期）。
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);

const STATUS = { queued: '思考中', producing: '制作中', done: '已完成', failed: '没做成', cancelled: '已取消' };
const fmt = (iso) => new Date(iso).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false });
const rid = (a) => encodeURIComponent(a.requestId);

const CSS = `
:root{--bg:#fff8ef;--card:#fff;--ink:#2b2118;--sub:#8a7a6a;--accent:#ff8a3d;--line:#f0e4d6;--tag:#fff1e4}
@media (prefers-color-scheme:dark){:root{--bg:#1c1712;--card:#29221b;--ink:#f6ece2;--sub:#b3a291;--line:#3a3027;--tag:#3a2b1e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 4px}.sub{color:var(--sub);font-size:13px;margin:0 0 16px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.card{background:var(--card);border-radius:16px;overflow:hidden;text-decoration:none;color:inherit;box-shadow:0 1px 0 var(--line)}
.cover{aspect-ratio:3/4;background:var(--tag) center/cover no-repeat;display:flex;align-items:center;justify-content:center;color:var(--sub);font-size:14px;text-align:center;padding:8px}
.card .t{padding:8px 10px 2px;font-weight:600;font-size:15px}.card .q{padding:0 10px 10px;color:var(--sub);font-size:12px}
.badge{display:inline-block;font-size:11px;padding:1px 8px;border-radius:99px;background:var(--tag);color:var(--accent)}
.ai{display:inline-block;font-size:11px;padding:1px 6px;border-radius:4px;border:1px solid var(--sub);color:var(--sub)}
video{width:100%;border-radius:16px;background:#000;aspect-ratio:9/16;max-height:78vh}
.box{background:var(--card);border-radius:16px;padding:14px 16px;margin-top:12px}.box b{display:block;margin-bottom:4px}
a.back{color:var(--accent);text-decoration:none;font-size:14px}.empty{color:var(--sub);text-align:center;padding:48px 0}
`;

const layout = (title, body, refresh) => `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">${refresh ? `<meta http-equiv="refresh" content="${refresh}">` : ''}
<meta name="robots" content="noindex"><title>${esc(title)}</title><style>${CSS}</style></head><body><main>${body}</main></body></html>`;

function card(a) {
  const v = a.video ?? {};
  const done = a.status === 'done';
  const label = done ? '' : esc(STATUS[a.status] ?? a.status) + (v.etaMinutes ? `<br>约 ${esc(v.etaMinutes)} 分钟` : '') + (a.status === 'failed' && v.failReason ? `<br><small>${esc(v.failReason)}</small>` : '');
  const inner = `<div class="cover"${done && v.coverUrl ? ` style="background-image:url('/c/${rid(a)}')"` : ''}>${label}</div>
<div class="t">${esc(v.title || a.question)}</div>
<div class="q">${done ? '' : `<span class="badge">${esc(STATUS[a.status] ?? a.status)}</span> `}${esc(fmt(a.createdAt))}</div>`;
  return done ? `<a class="card" href="/v/${rid(a)}">${inner}</a>` : `<div class="card">${inner}</div>`;
}

export function listPage(device, asks) {
  const busy = asks.some((a) => a.status === 'queued' || a.status === 'producing');
  const body = `<h1>孩子问的为什么</h1><p class="sub">设备 ${esc(device)} · 视频由 <span class="ai">AI 生成</span></p>
${asks.length ? `<div class="grid">${asks.map(card).join('')}</div>` : '<p class="empty">还没有问题，按住设备上的按钮问一个吧</p>'}`;
  return layout('孩子问的为什么', body, busy ? 30 : 0);
}

export function devicesPage(devs) {
  const body = `<h1>设备</h1><p class="sub">选择一台设备</p>${devs.length ? devs.map((d) =>
    `<div class="box"><a class="back" href="/d/${encodeURIComponent(d.device)}">${esc(d.device)}</a> · ${d.asks.length} 个问题</div>`).join('') : '<p class="empty">还没有设备提问</p>'}`;
  return layout('设备', body);
}

export function videoPage(a) {
  const v = a.video ?? {};
  const body = `<a class="back" href="/d/${encodeURIComponent(a.device)}">‹ 全部视频</a>
<h1>${esc(v.title || a.question)}</h1><p class="sub">孩子问：${esc(a.question)} · <span class="ai">AI 生成</span></p>
<video controls playsinline preload="metadata" src="/m/${rid(a)}"${v.coverUrl ? ` poster="/c/${rid(a)}"` : ''}></video>
${v.text ? `<div class="box">${esc(v.text)}</div>` : ''}
${v.experiment ? `<div class="box"><b>你可以试试</b>${esc(v.experiment)}</div>` : ''}
${v.followUpQuestion ? `<div class="box"><b>再想一想</b>${esc(v.followUpQuestion)}</div>` : ''}`;
  return layout(v.title || a.question, body);
}

export const notFoundPage = () => layout('找不到', '<p class="empty">找不到这个视频</p>');
