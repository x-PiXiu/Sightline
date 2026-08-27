#!/usr/bin/env bash
# 演示运行器：在指定端口起干净实例 → 跑交互 demo → 打印服务器日志尾部
# 用法: run_demo.sh [port]   （默认 8889，避开用户自己在 8888 的调试实例）
set -u
PORT="${1:-8889}"
cd "$(dirname "$0")/../../build" || exit 1
rm -f "server$PORT.log"
./sightline "$PORT" > "server$PORT.log" 2>&1 &
SERVER_PID=$!
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "SERVER FAILED TO START:"; cat "server$PORT.log"; exit 1
fi
echo "SERVER UP on :$PORT (pid $SERVER_PID)"
python3 -u ../scripts/verify/demo_interact.py "$PORT"
DEMO_RC=$?
kill -9 "$SERVER_PID" 2>/dev/null
echo "=== server log tail ==="
tail -8 "server$PORT.log"
exit $DEMO_RC
