#!/usr/bin/env bash
# 实时性证明：后台 tail -f 盯日志，同时跑七步演示，展示日志实时滚动
set -u
LOG=/mnt/e/workspace/Demo/C_Demo/Sightline/logs/server-8888.log
WC0=$(wc -l < "$LOG")
timeout 15 tail -f "$LOG" > /tmp/tail_capture.txt 2>&1 &
TPID=$!
sleep 0.5
python3 -u /mnt/e/workspace/Demo/C_Demo/Sightline/scripts/verify/demo_interact.py 8888 > /dev/null 2>&1
sleep 1
kill "$TPID" 2>/dev/null
echo "=== 演示期间 tail -f 实时捕获 $(wc -l < /tmp/tail_capture.txt) 行（演示前文件共 ${WC0} 行）==="
sed "s/\x1b\[[0-9;]*m//g" /tmp/tail_capture.txt | head -14
