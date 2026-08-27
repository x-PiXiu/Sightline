#!/usr/bin/env bash
# Sightline 压测运行器：最新构建 + 独立端口 + 提升fd上限 + 采集服务器指标
# 用法: run_bench.sh [phase1_n] [phase2_n] [phase3_n]   默认 500 / 1000 / 2000
set -u
PORT=8899
cd "$(dirname "$0")/../../build" || exit 1

# ★ 压测第一课：fd 软上限（默认 10240，是连接数的天然天花板）
ulimit -n 65535
echo "client/server fd limit = $(ulimit -n)"

./sightline $PORT > bench_server.log 2>&1 &
SPID=$!
sleep 1
if ! kill -0 $SPID 2>/dev/null; then echo "SERVER FAILED"; cat bench_server.log; exit 1; fi
echo "server pid=$SPID port=$PORT"

# 指标快照：RSS(kB) / CPU时钟滴答 / fd数
snap() {
    local tag="$1"
    local rss cpu fds
    rss=$(awk '/VmRSS/{print $2}' /proc/$SPID/status 2>/dev/null)
    cpu=$(awk '{print $14+$15}' /proc/$SPID/stat 2>/dev/null)
    fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)
    echo "[metric:$tag] rss=${rss}kB cpu_ticks=$cpu fds=$fds epoch=$(date +%s.%N)"
}

phase() {  # phase <label> <n> <hz> <dur>
    local label=$1 n=$2 hz=$3 dur=$4
    echo "===== PHASE $label: n=$n hz=$hz dur=${dur}s ====="
    snap "${label}_start"
    python3 -u ../scripts/bench/bench_client.py --port $PORT --n $n --hz $hz --duration $dur --ramp 300
    snap "${label}_end"
    sleep 3   # 等服务器清理断线
}

P1=${1:-500}; P2=${2:-1000}; P3=${3:-2000}

phase A "$P1" 2 20
phase B "$P2" 5 25
phase C "$P3" 2 25

echo "===== SERVER [stats] LINES ====="
grep "\[stats\]" bench_server.log | tail -12

kill -9 $SPID 2>/dev/null
echo "bench done"
