#!/usr/bin/env bash
# 服务器进程管理（开发期）：start / stop / status / restart
# stop 走 SIGINT —— 实测 D2-14 优雅关闭路径（广播 KICKED(3) 后退出）
set -euo pipefail
cd "$(dirname "$0")"

PID_FILE=/tmp/sightline_server.pid
LOG_FILE=/tmp/sightline_server.log

is_running() { [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; }

start() {
    if is_running; then echo "已在运行 pid=$(cat "$PID_FILE")"; return 0; fi
    rm -f "$LOG_FILE"
    ./sightline_server > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 0.3
    if is_running; then
        echo "✓ 启动 pid=$(cat "$PID_FILE") 日志=$LOG_FILE"
    else
        echo "✗ 启动失败"; tail -5 "$LOG_FILE"; return 1
    fi
}

stop() {
    if is_running; then
        local PID
        PID=$(cat "$PID_FILE")
        kill -INT "$PID"                                 # 优雅：广播 KICKED(3) 后自行退出
        for _ in $(seq 1 50); do is_running || break; sleep 0.1; done   # 最多等 5s
        if is_running; then echo "未响应 SIGINT，强制杀死"; kill -9 "$PID"; fi
        rm -f "$PID_FILE"
        echo "✓ 已停止"
    else
        rm -f "$PID_FILE"
        echo "未运行（清理残留 pid 文件）"
    fi
}

status() {
    if is_running; then echo "运行中 pid=$(cat "$PID_FILE")"; else echo "未运行"; fi
}

case "${1:-status}" in
    start)   start ;;
    stop)    stop ;;
    status)  status ;;
    restart) stop; start ;;
    *)       echo "用法: dev_server.sh [start|stop|status|restart]" ;;
esac
