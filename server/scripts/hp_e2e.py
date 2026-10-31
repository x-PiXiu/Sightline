#!/usr/bin/env python3
# 端到端血量链验证：A 建房开局，B 加入；A 朝 B 连开 4 枪 → 收 21 号 victimHp 应 75/50/25/0
# 用法: python3 hp_e2e.py [port]
import socket, struct, time, sys

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 28888
def conn(name):
    s = socket.create_connection((HOST, PORT), timeout=3)
    buf = b""
    def send(mid, p=b""): s.sendall(struct.pack("<HH", 2+len(p), mid)+p)
    def recv():
        nonlocal buf
        while len(buf) < 4: buf += s.recv(65536)
        (ln,) = struct.unpack("<H", buf[:2])
        while len(buf) < 2+ln: buf += s.recv(65536)
        (mid,) = struct.unpack("<H", buf[2:4]); pay = buf[4:2+ln]; buf = buf[2+ln:]
        return mid, pay
    send(4, bytes([len(name)])+name.encode())
    while True:
        m, p = recv()
        if m == 5: return send, recv, struct.unpack("<I", p[1:5])[0]

sA, rA, pidA = conn("shooterA")
sB, rB, pidB = conn("targetB")
sA(6, bytes([3])+b"room")
while True:
    m, p = rA()
    if m == 7: rid = struct.unpack("<I", p[1:5])[0]; break
sB(8, struct.pack("<I", rid))
while True:
    m, p = rB()
    if m == 9: break
sA(14)
while True:
    m, p = rA()
    if m == 15: break
for _ in range(20):
    rB()
seqA = 0
for shot in range(4):
    seqA += 1; sA(19, struct.pack("<I3f2fB", seqA, 420.0, 0.0, 90.0, 0.0, 0.0, 0))
    sB(19, struct.pack("<I3f2fB", shot+1, 500.0, 0.0, 90.0, 180.0, 0.0, 0))
    sA(20)
    hit = None
    deadline = time.monotonic()+2
    while time.monotonic() < deadline:
        m, p = rA()
        if m == 21:
            hit = (struct.unpack("<I", p[0:4])[0], p[4], struct.unpack("<I", p[5:9])[0], p[21]); break
    print(f"第{shot+1}枪: 21号 shooter={hit[0]} result={hit[1]} victim={hit[2]} victimHp={hit[3]}")
    time.sleep(0.12)
