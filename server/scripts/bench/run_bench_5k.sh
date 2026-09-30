#!/usr/bin/env bash
# 极限连接数测试：N 连接低频保活，验证容量与内存
# 用法: run_bench_5k.sh [n] [port]   默认 5000 / 8896
set -u
N=${1:-5000}; PORT=${2:-8896}
cd "$(dirname "$0")/../../build" || exit 1
ulimit -n 65535
./sightline $PORT > bench_5k.log 2>&1 &
SPID=$!
sleep 1
python3 -u ../scripts/bench/bench_client.py --port $PORT --n $N --hz 1 --duration 25 --ramp 500
sleep 3
echo "peak: rss=$(awk '/VmRSS/{print $2}' /proc/$SPID/status 2>/dev/null)kB fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)"
kill -9 $SPID 2>/dev/null
grep "\[stats\]" bench_5k.log | tail -1
