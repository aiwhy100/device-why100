#!/bin/sh
# 一分钟上手：只用 curl 提一个问题，等到做好，打印结果。
#   export AIWHY100_API_KEY=svf_live_…           # https://aiwhy100.com/app/ 「设置 → API Key」
#   sh examples/curl/quickstart.sh "为什么天空是蓝色的？"
# 本地模拟服务器（不花额度）：export AIWHY100_BASE_URL=http://127.0.0.1:8787
set -eu
Q="${1:-为什么天空是蓝色的？}"
BASE="${AIWHY100_BASE_URL:-https://api.aiwhy100.com}/api/app/v1"
: "${AIWHY100_API_KEY:?先 export AIWHY100_API_KEY=svf_live_…}"
AUTH="Authorization: Bearer $AIWHY100_API_KEY"

# 1. 提交（channel=kids：给 5–12 岁孩子讲，约 100 秒）。问题里的引号要转义成 \"
BODY=$(printf '{"question":"%s","channel":"kids"}' "$(printf '%s' "$Q" | sed 's/\\/\\\\/g; s/"/\\"/g')")
R=$(curl -sS -X POST "$BASE/videos" -H "$AUTH" -H 'Content-Type: application/json' -d "$BODY")
echo "$R"
ID=$(printf '%s' "$R" | grep -o '"id":"[^"]*"' | head -1 | cut -d'"' -f4)
[ -n "$ID" ] || { echo "提交失败（见上面的 message）" >&2; exit 1; }

# 2. 轮询，直到 done / failed（正式接口建议 15–30 秒查一次）
while :; do
  sleep "${POLL_SECONDS:-20}"
  R=$(curl -sS "$BASE/videos/$ID" -H "$AUTH")
  case "$R" in
    *'"status":"done"'*|*'"status":"failed"'*|*'"status":"cancelled"'*) break ;;
  esac
  printf '%s\n' "$R" | sed -n 's/.*"statusText":"\([^"]*\)".*/还在做：\1/p'
done
echo "$R"
