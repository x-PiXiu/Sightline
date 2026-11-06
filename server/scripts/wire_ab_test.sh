#!/usr/bin/env bash
# wire_ab_test.sh —— ep08 5b：双模式 wire 对拍（lite vs full 唯一帧集 diff）
set -uo pipefail
cd "$(dirname "$0")/.."

run_mode() {
    local mode="$1" out="$2"
    if [ "$mode" = full ]; then
        SIGHTLINE_MYSQL_DSN='127.0.0.1:3306:sightline:sightline:sightline' \
        SIGHTLINE_REDIS_URI='127.0.0.1:6379:123456' \
        SIGHTLINE_MONGO_URI='mongodb://127.0.0.1:27017' \
        ./sightline_server --db-profile=full > "/tmp/srv_${mode}.log" 2>&1 &
    else
        ./sightline_server > "/tmp/srv_${mode}.log" 2>&1 &
    fi
    local pid=$!
    sleep 1.5
    python3 scripts/wire_replay.py > "$out" 2>"/tmp/wire_${mode}.err"
    local rc=$?
    kill -INT "$pid" 2>/dev/null
    sleep 1.5
    kill -9 "$pid" 2>/dev/null
    echo "${mode}: rc=${rc} lines=$(wc -l < "$out")"
    if [ "$rc" -ne 0 ]; then
        echo "--- ${mode} stderr ---"
        tail -5 "/tmp/wire_${mode}.err"
        echo "--- server log tail ---"
        tail -8 "/tmp/srv_${mode}.log"
        return "$rc"
    fi
    return 0
}

run_mode lite /tmp/wire_lite.txt || exit 1
run_mode full /tmp/wire_full.txt || exit 1

echo "=== diff（空=对拍通过）==="
if diff /tmp/wire_lite.txt /tmp/wire_full.txt; then
    echo "IDENTICAL ✓（§5.7.5 验收门 4 通过）"
else
    echo "✗ 帧集合不一致"
    exit 1
fi
echo "=== 帧清单（msgid × 种类）==="
cut -d' ' -f1 /tmp/wire_full.txt | sort | uniq -c
