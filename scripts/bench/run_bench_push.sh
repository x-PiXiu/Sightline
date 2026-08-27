#!/usr/bin/env bash
# 容量上探：多 loadbot 实例 × 多环回 IP，突破单 IP 临时端口池(~28k)限制
# 用法: run_bench_push.sh <总连接数> [port]     例: run_bench_push.sh 45000
# 原理: 127.0.0.1/.2/.3 各自拥有独立的端口元组空间 → 3×28k 可用
set -u
TOTAL=${1:-30000}; PORT=${2:-8894}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT/build" || exit 1
ulimit -n 65535     # 服务器与 loadbot 共享本 shell 的上限

INSTANCES=3
PER=$((TOTAL / INSTANCES))
IPS=("127.0.0.1" "127.0.0.2" "127.0.0.3")

EXTPID=${EXTPID:-0}   # 传外部服务器PID则压测之（不启停目标）
if [ "$EXTPID" = "0" ]; then
    rm -f "server_push$PORT.log"
    ./sightline $PORT > "server_push$PORT.log" 2>&1 &
    SPID=$!
    sleep 1
    kill -0 $SPID 2>/dev/null || { echo "SERVER FAILED"; cat "server_push$PORT.log"; exit 1; }
else
    SPID=$EXTPID
fi
echo "server pid=$SPID port=$PORT, target=$TOTAL ($PER x $INSTANCES IPs)"

monitor() {   # 后台每 2s 采样一次资源
    while kill -0 $SPID 2>/dev/null; do
        echo "$(date +%T) rss=$(awk '/VmRSS/{print $2}' /proc/$SPID/status 2>/dev/null)kB \
cpu=$(awk '{print $14+$15}' /proc/$SPID/stat 2>/dev/null) fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)"
        sleep 2
    done
} > "push_monitor$PORT.log" 2>&1 &
MON=$!

# 3 实例并发压测（各绑不同环回 IP）；显式收集 PID 再 wait（jobs -p 不可靠）
DUR=45
declare -a BOTS=()
for i in 0 1 2; do
    "$ROOT/loadbot/loadbot" -host "${IPS[$i]}" -port $PORT -n $PER -hz 1 -dur $DUR -ramp 1500 \
        > "push_bot$i.log" 2>&1 &
    BOTS+=($!)
done
wait "${BOTS[@]}"
sleep 2
kill $MON 2>/dev/null

echo "=== loadbot 汇总 ==="
for i in 0 1 2; do tail -1 "push_bot$i.log"; done
echo "=== 资源峰值（连接保持期）==="
sort -t= -k3 -n "push_monitor$PORT.log" | tail -3
echo "=== 服务器 stats ==="
grep "\[stats\]" "server_push$PORT.log" 2>/dev/null | tail -2
[ "$EXTPID" = "0" ] && kill -9 $SPID 2>/dev/null
