#!/usr/bin/env bash
# 一键冒烟：起服 → python 双客户端全流程 → 关服 → 打印日志（带超时保护）
set -u
cd "$(dirname "$0")/../../build" || exit 1
rm -f server.log
./sightline 8891 > server.log 2>&1 &
SERVER_PID=$!
sleep 1
timeout 30 python3 -u ../scripts/verify/smoke_test.py 8891
SMOKE_RC=$?
kill -9 "$SERVER_PID" 2>/dev/null
echo "smoke_exit=$SMOKE_RC"
echo "=== server.log ==="
cat server.log
exit $SMOKE_RC
