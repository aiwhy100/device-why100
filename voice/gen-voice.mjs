#!/usr/bin/env node
// 生成设备上的预置提示语（吉祥物「小奇」的声音）。改 prompts.json 里的文字后运行：
//   node voice/gen-voice.mjs          只生成新增 / 改过的句子
//   node voice/gen-voice.mjs --all    全部重新生成
// 输出 voice/out/<id>.wav：16kHz 单声道 16 位 PCM（板子直接播，固件编译时打包进 Flash）
// 依赖：uvx（临时跑 edge-tts）、ffmpeg
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const cfg = JSON.parse(fs.readFileSync(path.join(HERE, 'prompts.json'), 'utf8'));
const OUT = path.join(HERE, 'out');
const STAMPS = path.join(OUT, '.keys.json');
const all = process.argv.includes('--all');
fs.mkdirSync(OUT, { recursive: true });
const stamps = (() => { try { return JSON.parse(fs.readFileSync(STAMPS, 'utf8')); } catch { return {}; } })();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'voice-'));

let made = 0;
for (const p of cfg.prompts) {
  const voice = p.voice ?? cfg.voice, rate = p.rate ?? cfg.rate;
  const key = createHash('sha1').update([voice, rate, p.text].join('|')).digest('hex').slice(0, 12);
  const out = path.join(OUT, `${p.id}.wav`);
  if (!all && stamps[p.id] === key && fs.existsSync(out)) continue;
  const mp3 = path.join(tmp, `${p.id}.mp3`);
  execFileSync('uvx', ['edge-tts', '--voice', voice, `--rate=${rate}`, '--text', p.text, '--write-media', mp3], { stdio: 'pipe' });
  // 去掉首尾静音，响度统一；小喇叭上留一点余量（TP -2）
  execFileSync('ffmpeg', ['-y', '-v', 'error', '-i', mp3,
    '-af', 'silenceremove=start_periods=1:start_threshold=-50dB,areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse,loudnorm=I=-16:TP=-2',
    '-ac', '1', '-ar', '16000', '-c:a', 'pcm_s16le', out]);
  stamps[p.id] = key;
  made++;
  console.log(`✓ ${p.id}  ${p.text}`);
}
const ids = new Set(cfg.prompts.map((p) => p.id));
for (const f of fs.readdirSync(OUT)) if (f.endsWith('.wav') && !ids.has(f.slice(0, -4))) { fs.unlinkSync(path.join(OUT, f)); console.log(`- 删除 ${f}`); }
for (const k of Object.keys(stamps)) if (!ids.has(k)) delete stamps[k];
fs.writeFileSync(STAMPS, JSON.stringify(stamps, null, 2) + '\n');
fs.rmSync(tmp, { recursive: true, force: true });
console.log(made ? `生成 ${made} 条 → ${OUT}` : '没有变化');
