#!/usr/bin/env python3
# 冒烟测试：两个客户端走完整对战流程
# 登录 → 进房(×2) → RoomStart → 互见移动 → A 开火命中 B → 验证 HitEvent
import socket, struct, sys, time

HOST, PORT = "127.0.0.1", 8888

def frame(msg_id: int, payload: bytes) -> bytes:
    return struct.pack("<HH", len(payload), msg_id) + payload

def login_pkt(name: str) -> bytes:
    b = name.encode()
    return frame(1, struct.pack("<H", len(b)) + b)

JOIN, MOVE, FIRE = frame(3, b""), None, None

class Client:
    def __init__(self, name):
        self.s = socket.create_connection((HOST, PORT), timeout=3)
        self.buf = b""
        self.s.sendall(login_pkt(name))
        self.pid = None
        ev = self.recv()          # LoginAck
        assert ev and ev[0] == 2, f"expected LoginAck, got {ev}"
        self.pid = struct.unpack("<I", ev[1])[0]

    def recv(self, timeout=2.0):
        self.s.settimeout(timeout)
        try:
            while True:
                # 尝试解一帧
                if len(self.buf) >= 4:
                    ln, mid = struct.unpack("<HH", self.buf[:4])
                    if len(self.buf) >= 4 + ln:
                        payload = self.buf[4:4+ln]
                        self.buf = self.buf[4+ln:]
                        return mid, payload
                data = self.s.recv(4096)
                if not data:
                    return None
                self.buf += data
        except socket.timeout:
            return None

def move_pkt(pos, yaw=0.0):
    return frame(5, struct.pack("<3f", *pos) + struct.pack("<f", yaw))

def fire_pkt(origin, direction):
    return frame(7, struct.pack("<3f", *origin) + struct.pack("<3f", *direction))

a, b = Client("Alice"), Client("Bob")
print(f"[1] logged in: A={a.pid} B={b.pid}")

a.s.sendall(JOIN)
ev = a.recv(); assert ev is None or ev[0] != 4, "单人不应开战"
b.s.sendall(JOIN)

start = b.recv()
assert start and start[0] == 4, f"expected RoomStart, got {start}"
count = struct.unpack("<I", start[1][:4])[0]
players = struct.unpack(f"<{count}I", start[1][4:4+count*4])
print(f"[2] RoomStart: players={players}")
assert set(players) == {a.pid, b.pid}

# A 也有一份 RoomStart 排在队列里，先消费掉
a_start = a.recv()
assert a_start and a_start[0] == 4, f"A 应也收到 RoomStart, got {a_start}"

# 移动：A 到原点，B 到 A 前方 10 米（服务器采纳后用于命中判定）
a.s.sendall(move_pkt((0.0, 0.0, 0.0)))
time.sleep(0.1)
b.s.sendall(move_pkt((10.0, 0.0, 0.0)))
time.sleep(0.2)

# A 收到 B 的移动广播
ev = a.recv()
assert ev and ev[0] == 6, f"A 应收到 B 的 MoveEvent, got {ev}"
mv_pid, = struct.unpack("<I", ev[1][:4])
print(f"[3] A saw B move: pid={mv_pid}")
assert mv_pid == b.pid

# A 开火：原点朝 +x → 应命中 B
a.s.sendall(fire_pkt((0.0, 0.0, 0.0), (1.0, 0.0, 0.0)))
ev = a.recv()
assert ev and ev[0] == 8, f"A 应收到 HitEvent, got {ev}"
shooter, victim, dmg, hp, dead = struct.unpack("<IIIIB", ev[1][:17])
print(f"[4] HIT: shooter={shooter} victim={victim} dmg={dmg} hp={hp} dead={dead}")
assert shooter == a.pid and victim == b.pid and hp == 75 and not dead

# 心跳/Pong RTT 通道
a.s.sendall(frame(10, struct.pack("<Q", 123456)))
ev = a.recv()
assert ev and ev[0] == 11, f"expected Pong, got {ev}"
t, = struct.unpack("<Q", ev[1])
assert t == 123456
print(f"[5] Pong RTT channel OK (t={t})")

print("SMOKE TEST: ALL PASSED")
