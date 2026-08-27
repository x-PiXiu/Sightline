#!/usr/bin/env bash
# Go loadbot 压测运行器：最新构建 + 独立端口 + fd 上限放开
# 用法: run_bench_go.sh <n> <hz> <dur> [port]    例: run_bench_go.sh 10000 1 30
set -u
N=${1:-1000}; HZ=${2:-2}; DUR=${3:-30}; PORT=${4:-8892}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT/build" || exit 1
ulimit -n 65535
rm -f "server_go$PORT.log"
./sightline $PORT > "server_go$PORT.log" 2>&1 &
SPID=$!
sleep 1
kill -0 $SPID 2>/dev/null || { echo "SERVER FAILED"; cat "server_go$PORT.log"; exit 1; }

snap() {
    echo "[metric:$1] rss=$(awk '/VmRSS/{print $2}' /proc/$SPID/status 2>/dev/null)kB \
cpu_ticks=$(awk '{print $14+$15}' /proc/$SPID/stat 2>/dev/null) fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)"
}

snap start
"$ROOT/loadbot/loadbot" -port $PORT -n $N -hz $HZ -dur $DUR -ramp 2000
BOT_RC=$?
sleep 2
snap end
echo "=== server stats ==="
grep "\[stats\]" "server_go$PORT.log" | tail -3
kill -9 $SPID 2>/dev/null
exit $BOT_RC
