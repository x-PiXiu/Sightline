#!/usr/bin/env bash
# 多 Reactor 拓扑自证：启动日志显示各线程 ID + /proc 线程数
set -u
cd "$(dirname "$0")/../../build" || exit 1
rm -f threads_demo.log
./sightline 8893 > threads_demo.log 2>&1 &
SPID=$!
sleep 1.5
echo "=== 启动日志（时间戳后的数字 = 线程ID）==="
sed "s/\x1b\[[0-9;]*m//g" threads_demo.log | grep -E "GameServer started|IO loop thread" | cut -c1-120
echo "=== 进程线程数（期望 3 = 主 + 2 IO）==="
ls /proc/$SPID/task | wc -l
kill -9 $SPID 2>/dev/null
