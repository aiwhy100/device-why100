#!/usr/bin/env python3
"""AI为什么100 设备端示例（只用 Python 标准库，树莓派 / 电脑都能跑）。

把一个「为什么」问题（文字、录音文件或现场录音）交给 AI为什么100，几分钟后拿到科普动画视频。

    export AIWHY100_API_KEY=svf_live_xxxxxxxx        # aiwhy100.com/app 「设置 → API Key」
    python3 why.py "为什么天空是蓝色的？"              # 文字提问
    python3 why.py --wav q.wav                        # 录音文件（16kHz 单声道 16 位 WAV）→ 识别 → 提问
    python3 why.py --record 5                         # 用 arecord 现场录 5 秒（树莓派 + USB 麦克风）
    python3 why.py --loop                             # 按回车录音，再按回车结束，一直循环（像一个按钮）
    python3 why.py --list                             # 看最近的视频

本地联调不花额度：先 `node mock/server.mjs`，再 `export AIWHY100_BASE_URL=http://127.0.0.1:8787`。
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import wave

BASE = os.environ.get("AIWHY100_BASE_URL", "https://api.aiwhy100.com").rstrip("/")
API = f"{BASE}/api/app/v1"
KEY = os.environ.get("AIWHY100_API_KEY", "")


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


def call(method: str, path: str, body=None, raw: bytes = None, timeout: int = 30):
    headers = {"Authorization": f"Bearer {KEY}"}
    data = None
    if raw is not None:
        data = raw
        headers["Content-Type"] = "application/octet-stream"
    elif body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(API + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        try:
            msg = json.load(e).get("message", "")
        except Exception:
            msg = ""
        raise ApiError(e.code, msg or f"HTTP {e.code}") from None


def recognize(pcm: bytes) -> str:
    """16kHz 单声道 16 位 PCM → 问句；没听清返回空串"""
    r = call("POST", "/asr?format=pcm16", raw=pcm)
    return r.get("question", "")


def read_wav(path: str) -> bytes:
    with wave.open(path, "rb") as w:
        if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit("需要 16kHz、单声道、16 位的 WAV（ffmpeg -i in.m4a -ac 1 -ar 16000 -c:a pcm_s16le q.wav）")
        return w.readframes(w.getnframes())


def record(seconds: float = None) -> bytes:
    """用 arecord 录音；seconds 为空时按回车结束"""
    if not shutil.which("arecord"):
        sys.exit("找不到 arecord（树莓派：sudo apt install alsa-utils）；电脑上请用 --wav")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "q.wav")
        cmd = ["arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1"]
        if seconds:
            print(f"🎙  录音 {seconds} 秒……")
            subprocess.run(cmd + ["-d", str(int(seconds)), path], check=True)
        else:
            p = subprocess.Popen(cmd + [path])
            input("🎙  正在录音，说完按回车……")
            p.terminate()
            p.wait()
        return read_wav(path)  # 录音只在内存里用，临时文件随目录一起删掉


def ask(question: str, channel: str, match: bool):
    """先查「小奇讲过啦」（不扣额度），没有再提交制作。返回视频对象"""
    if match:
        m = call("POST", "/questions/match", {"question": question}).get("match")
        if m:
            v = m["video"]
            if not m.get("own"):
                v = call("POST", f"/videos/{v['id']}/save")
            print(f"✨ 这个问题小奇讲过啦，不扣额度：「{v.get('title') or v['question']}」")
            return v
    v = call("POST", "/videos", {"question": question, "channel": channel})
    print(f"📮 已提交「{v['question']}」，{v['statusText']}，大约 {v.get('etaMinutes') or '?'} 分钟")
    return v


def wait(v, every: int = 20):
    """轮询到完成；思考中 / 制作中建议 15–30 秒查一次"""
    last = None
    while v["status"] in ("queued", "producing"):
        if v["statusText"] != last:
            last = v["statusText"]
            print(f"⏳ {last}" + (f"，大约还要 {v['etaMinutes']} 分钟" if v.get("etaMinutes") else ""))
        time.sleep(every)
        v = call("GET", f"/videos/{v['id']}")
    return v


def show(v):
    if v["status"] == "done":
        links = v.get("links") or {}
        print(f"🎬 做好啦：「{v.get('title')}」")
        print(f"   播放（1 小时内有效）：{links.get('video')}")
        print(f"   分享给家长（24 小时内有效）：{links.get('share')}")
        print("   也可以在手机上打开 https://aiwhy100.com/app/ 登录同一个账号，在「我的视频」里看")
    else:
        print(f"❌ {v['statusText']}：{v.get('failReason') or ''}")


def handle(question: str, args):
    if not question:
        print("🤔 没听清，再说一遍吧")
        return
    print(f"❓ {question}")
    try:
        v = ask(question, args.channel, not args.no_match)
        if args.no_wait:
            print(f"   视频 ID：{v['id']}，稍后用 GET /videos/{v['id']} 查结果")
            return
        show(wait(v, args.every))
    except ApiError as e:
        hint = {
            401: "API Key 不对：检查 AIWHY100_API_KEY",
            402: "额度用完了：在 aiwhy100.com/app 里看看",
            422: "这个问题不适合做成视频，换一个吧",
            429: "太频繁了：同时最多 2 个在做、每小时最多 5 个",
        }.get(e.status, "")
        print(f"⚠️  {e}" + (f"（{hint}）" if hint else ""))


def main():
    ap = argparse.ArgumentParser(description="AI为什么100 设备端示例", formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("question", nargs="?", help="文字问题")
    ap.add_argument("--wav", help="录音文件：16kHz 单声道 16 位 WAV")
    ap.add_argument("--record", type=float, metavar="秒", help="用 arecord 录几秒")
    ap.add_argument("--loop", action="store_true", help="按回车录音，循环提问")
    ap.add_argument("--list", action="store_true", help="看最近的视频")
    ap.add_argument("--channel", default="kids", choices=["kids", "web"], help="kids：给 5–12 岁孩子，约 100 秒（默认）；web：普通，60–90 秒")
    ap.add_argument("--no-match", action="store_true", help="不查「小奇讲过啦」，直接做新的")
    ap.add_argument("--no-wait", action="store_true", help="提交后不等结果")
    ap.add_argument("--every", type=int, default=20, help="轮询间隔（秒）")
    args = ap.parse_args()

    if not KEY:
        sys.exit("先设置 API Key：export AIWHY100_API_KEY=svf_live_…（在 https://aiwhy100.com/app/ 「设置 → API Key」创建）")

    try:
        if args.list:
            for v in call("GET", "/videos?limit=10")["items"]:
                print(f"{v['statusText']:　<6} {v['question']}  {v['id']}")
        elif args.loop:
            while True:
                input("\n按回车开始录音（Ctrl+C 退出）")
                handle(recognize(record()), args)
        elif args.record:
            handle(recognize(record(args.record)), args)
        elif args.wav:
            handle(recognize(read_wav(args.wav)), args)
        elif args.question:
            handle(args.question.strip(), args)
        else:
            ap.print_help()
    except ApiError as e:
        sys.exit(f"⚠️  {e.status} {e}")
    except urllib.error.URLError as e:
        sys.exit(f"⚠️  连不上 {BASE}：{e.reason}")
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
