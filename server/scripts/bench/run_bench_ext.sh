#!/usr/bin/env bash
# 外部目标压测：对一个已在运行的服务器实例执行完整四阶段压测（不启停目标）
# 用法: run_bench_ext.sh <port> <server_pid>
set -u
PORT=${1:?need port}; SPID=${2:?need server pid}
cd "$(dirname "$0")/../../build" || exit 1
ulimit -n 65535
echo "target: port=$PORT pid=$SPID  fd_limit=$(ulimit -n)"
[ -d /proc/$SPID ] || { echo "target pid not alive"; exit 1; }

snap() {
    local tag="$1"
    local rss cpu fds
    rss=$(awk '/VmRSS/{print $2}' /proc/$SPID/status 2>/dev/null)
    cpu=$(awk '{print $14+$15}' /proc/$SPID/stat 2>/dev/null)
    fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)
    echo "[metric:$tag] rss=${rss}kB cpu_ticks=$cpu fds=$fds epoch=$(date +%s.%N)"
}

phase() {
    local label=$1 n=$2 hz=$3 dur=$4
    echo "===== PHASE $label: n=$n hz=$hz dur=${dur}s ====="
    snap "${label}_start"
    python3 -u ../scripts/bench/bench_client.py --port $PORT --n $n --hz $hz --duration $dur --ramp 400
    snap "${label}_end"
    sleep 3
    [ -d /proc/$SPID ] || { echo "!!! SERVER DIED after phase $label"; exit 2; }
}

phase A 500 2 20
phase B 1000 5 25
phase C 2000 2 25
phase D 5000 1 25

echo "===== ALL PHASES DONE, SERVER ALIVE ====="
snap final
