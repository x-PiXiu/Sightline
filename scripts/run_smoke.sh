#!/usr/bin/env bash
# 一键冒烟：起服 → python 双客户端全流程 → 关服 → 打印日志（带超时保护）
set -u
cd "$(dirname "$0")/../build" || exit 1
rm -f server.log
./sightline > server.log 2>&1 &
SERVER_PID=$!
sleep 1
timeout 30 python3 -u ../scripts/smoke_test.py
SMOKE_RC=$?
kill -9 "$SERVER_PID" 2>/dev/null
echo "smoke_exit=$SMOKE_RC"
echo "=== server.log ==="
cat server.log
exit $SMOKE_RC
