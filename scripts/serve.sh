#!/usr/bin/env bash
# 长驻启动 Sightline：nohup 分离运行，日志落盘（Windows 可直接打开），PID 记录便于停止
# 用法:  wsl bash scripts/serve.sh [port] [debug]   （debug = 实时观察连接进出的 DEBUG 日志）
# 看日志: tail -f logs/server-<port>.log         （WSL 内；Windows 侧见文件同路径 E:\）
# 停止:  kill $(cat logs/server-<port>.pid)
set -u
PORT=${1:-8888}
MODE=${2:-}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/build" || exit 1
ulimit -n 65535
mkdir -p "$ROOT/logs"

# 端口已被本服务占用则拒绝重复启动
if [ -f "$ROOT/logs/server-$PORT.pid" ] && kill -0 "$(cat "$ROOT/logs/server-$PORT.pid")" 2>/dev/null; then
    echo "already running on :$PORT (pid $(cat "$ROOT/logs/server-$PORT.pid"))"; exit 0
fi

nohup ./sightline "$PORT" $MODE > "$ROOT/logs/server-$PORT.log" 2>&1 &
echo $! > "$ROOT/logs/server-$PORT.pid"
sleep 1
PID=$(cat "$ROOT/logs/server-$PORT.pid")
if kill -0 "$PID" 2>/dev/null; then
    echo "Sightline running on :$PORT  (pid $PID, io_threads=2, 异步日志)"
    echo "  日志文件: $ROOT/logs/server-$PORT.log"
    echo "  实时查看: wsl tail -f /mnt/e/workspace/Demo/C_Demo/Sightline/logs/server-$PORT.log"
    echo "  停止服务: wsl bash -c 'kill \$(cat /mnt/e/workspace/Demo/C_Demo/Sightline/logs/server-$PORT.pid)'"
else
    echo "START FAILED:"; tail -5 "$ROOT/logs/server-$PORT.log"; rm -f "$ROOT/logs/server-$PORT.pid"; exit 1
fi
