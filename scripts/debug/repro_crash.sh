#!/usr/bin/env bash
# gdb 下跑断线风暴，抓崩溃栈
set -u
PORT=8898
cd "$(dirname "$0")/../../build" || exit 1
make -j4 > /dev/null 2>&1
ulimit -c unlimited
gdb -batch -ex run -ex "thread apply all bt 15" --args ./sightline $PORT > gdb_out.log 2>&1 &
GDBPID=$!
sleep 2
python3 -u ../scripts/debug/bench_disconnect_storm.py 300 $PORT
sleep 3
echo "=== gdb result (tail) ==="
tail -40 gdb_out.log
kill -9 $GDBPID 2>/dev/null
