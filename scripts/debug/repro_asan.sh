#!/usr/bin/env bash
# ASan 版服务器 + 流量风暴复现：内存错误会直接打印报告（无需等到崩溃）
set -u
PORT=8897
cd "$(dirname "$0")/../../build" || exit 1
# ASan 构建（独立二进制，不动原产物）
g++ -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer -O1 \
    -I../include ../main.cpp ../src/net/*.cpp ../src/logger/*.cpp \
    -lpthread -o sightline_asan 2> asan_build.log || { head -20 asan_build.log; exit 1; }
ulimit -n 65535
ASAN_OPTIONS=abort_on_error=0:detect_leaks=0 ./sightline_asan $PORT > asan_server.log 2>&1 &
SPID=$!
sleep 2
python3 -u ../scripts/debug/bench_crash_v2.py $PORT 400 4
sleep 2
echo "=== ASan 报告检索 ==="
grep -n -A 25 "ERROR: AddressSanitizer" asan_server.log | head -60 || echo "(无 ASan 报告)"
echo "=== server log tail ==="
tail -5 asan_server.log
kill -9 $SPID 2>/dev/null
