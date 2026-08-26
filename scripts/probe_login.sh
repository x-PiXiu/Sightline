#!/usr/bin/env bash
# 诊断探针：起服 → 单客户端 Login → 打印收包与服务器日志
set -u
cd "$(dirname "$0")/../build" || exit 1
make -j4 > /dev/null 2>&1
rm -f server.log
./sightline > server.log 2>&1 &
SERVER_PID=$!
sleep 1
python3 -u - <<'PYEOF'
import socket, struct
s = socket.create_connection(("127.0.0.1", 8888), timeout=2)
import time; time.sleep(0.5)  # 确保数据在 EPOLL_CTL_ADD 之后到达
name = b"Alice"
p = struct.pack("<H", len(name)) + name
s.sendall(struct.pack("<HH", len(p), 1) + p)
s.settimeout(2)
try:
    print("recv:", s.recv(64).hex())
except Exception as e:
    print("recv fail:", e)
PYEOF
kill -9 "$SERVER_PID" 2>/dev/null
sleep 0.3
echo "=== server.log ==="
cat server.log
