// 极简存储：一个 JSON 文件（DATA_DIR/db.json）。只存问题文字和视频信息，不存录音。
import fs from 'node:fs';
import path from 'node:path';

/**
 * @typedef {object} Ask
 * @property {string} requestId   网关自己的编号（也是合作厂商接口的 requestId）
 * @property {string} device      设备 ID（MAC，小写带冒号）
 * @property {string} question
 * @property {string} [videoId]
 * @property {string} status      queued / producing / done / failed / cancelled
 * @property {object} [video]     统一格式的视频对象（见 upstream.js normalize）
 * @property {boolean} notified   设备是否已经播报过结果
 * @property {boolean} [found]    「小奇讲过啦」直接拿到的成片
 * @property {string} createdAt
 * @property {number} [nextPollAt]
 */

export function createStore(dir) {
  const file = path.join(dir, 'db.json');
  /** @type {{ asks: Ask[] }} */
  let db = { asks: [] };
  try {
    db = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {}

  return {
    save() {
      fs.mkdirSync(dir, { recursive: true });
      const tmp = `${file}.tmp`;
      fs.writeFileSync(tmp, JSON.stringify(db, null, 2));
      fs.renameSync(tmp, file);
    },
    all: () => db.asks,
    byDevice: (device) => db.asks.filter((a) => a.device === device),
    find: (requestId) => db.asks.find((a) => a.requestId === requestId),
    findByVideo: (videoId) => db.asks.find((a) => a.videoId === videoId),
    add(ask) {
      db.asks.unshift(ask);
      if (db.asks.length > 5000) db.asks.length = 5000;
      this.save();
      return ask;
    },
    devices: () => [...new Set(db.asks.map((a) => a.device))],
  };
}
