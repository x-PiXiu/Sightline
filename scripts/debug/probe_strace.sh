#!/usr/bin/env bash
# strace 诊断：观察 epoll_ctl / epoll_wait 的真实系统调用序列
set -u
cd "$(dirname "$0")/../../build" || exit 1
rm -f strace.log
strace -f -tt -e trace=epoll_create1,epoll_ctl,epoll_wait,accept4,accept,read,write -o strace.log ./sightline > server.log 2>&1 &
SERVER_PID=$!
sleep 1
python3 -u - <<'PYEOF' >/dev/null 2>&1
import socket, struct, time
s = socket.create_connection(("127.0.0.1", 8888), timeout=2)
time.sleep(0.3)
name = b"Alice"
p = struct.pack("<H", len(name)) + name
s.sendall(struct.pack("<HH", len(p), 1) + p)
time.sleep(0.7)
PYEOF
kill -9 "$SERVER_PID" 2>/dev/null
sleep 0.5
echo "=== epoll_ctl 序列 ==="
grep -E "epoll_ctl" strace.log | tail -15
echo "=== epoll_wait 返回非零（有事件）的最后几次 ==="
grep -E "epoll_wait.*= [1-9]" strace.log | tail -8
echo "=== accept 相关 ==="
grep -E "accept" strace.log | tail -5
