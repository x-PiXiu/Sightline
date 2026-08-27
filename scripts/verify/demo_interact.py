#!/usr/bin/env python3
# 交互演示：完整走一遍对战流程，打印每条消息的「原始字节(hex) + 解析结果」
import socket, struct, time, sys

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8888

NAMES = {
    1: "Login", 2: "LoginAck", 3: "JoinRoom", 4: "RoomStart",
    5: "Move", 6: "MoveEvt", 7: "Fire", 8: "Hit", 9: "GameOver",
    10: "Ping", 11: "Pong", 12: "Kick", 13: "PlayerLeft", 14: "Respawn",
}

def hexs(b): return " ".join(f"{x:02x}" for x in b)

def frame(msg_id, payload):
    return struct.pack("<HH", len(payload), msg_id) + payload

def parse(mid, p):
    """按协议表解析 payload 为可读文本"""
    f = lambda o=0: struct.unpack("<f", p[o:o+4])[0]
    u = lambda o=0: struct.unpack("<I", p[o:o+4])[0]
    if mid == 2:  return f"playerId={u()}"
    if mid == 4:
        n = u(); ids = struct.unpack(f"<{n}I", p[4:4+4*n])
        return f"count={n} players={ids}"
    if mid == 6:  return f"pid={u()} pos=({f(4):.1f},{f(8):.1f},{f(12):.1f}) yaw={f(16):.2f}"
    if mid == 8:  return f"shooter={u()} victim={u()} dmg={u(8)} victimHp={u(12)} dead={p[16]}"
    if mid == 9:  return f"winner={u()}"
    if mid == 11: return f"clientTime={struct.unpack('<Q', p)[0]}"
    if mid == 13: return f"leftPid={u()}"
    if mid == 14: return f"pid={u()} pos=({f(4):.1f},{f(8):.1f},{f(12):.1f}) hp={u(16)}"
    return f"({len(p)}B)"

class Client:
    def __init__(self, name):
        self.name, self.s, self.buf, self.pid = name, socket.create_connection((HOST, PORT), timeout=3), b"", None
        self.tx(1, struct.pack("<H", len(name)) + name.encode())
        mid, p = self.rx()
        self.pid = struct.unpack("<I", p)[0]

    def tx(self, mid, payload, note=""):
        wire = frame(mid, payload)
        self.s.sendall(wire)
        print(f"  TX {self.name}→S [{NAMES.get(mid,mid):9}] {hexs(wire)}{('  # ' + note) if note else ''}")

    def rx(self, timeout=2.0):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 4:
                ln, mid = struct.unpack("<HH", self.buf[:4])
                if len(self.buf) >= 4 + ln:
                    p, self.buf = self.buf[4:4+ln], self.buf[4+ln:]
                    print(f"  RX S→{self.name} [{NAMES.get(mid,mid):9}] {hexs(frame(mid,p))}  ⇒ {parse(mid,p)}")
                    return mid, p
            d = self.s.recv(4096)
            if not d: return None, None
            self.buf += d

step = lambda t: print(f"\n{'='*62}\n步骤 {t}\n{'='*62}")

a = Client("Alice"); b = Client("Bob")

step("1. 登录（Login → LoginAck）")
print(f"  Alice 拿到 playerId={a.pid}，Bob 拿到 playerId={b.pid}")

step("2. 进房与匹配（JoinRoom ×2 → RoomStart）")
a.tx(3, b"", "Alice 先排队，单人不开战")
time.sleep(0.2)
b.tx(3, b"", "Bob 进房 → 人齐自动开战")
b.rx(); a.rx()   # 双方各收一份 RoomStart

step("3. 移动同步（Move → 服务器采纳 → 转播 MoveEvt）")
a.tx(5, struct.pack("<3f", 0,0,0), "Alice 移动到原点")
time.sleep(0.1)
b.tx(5, struct.pack("<3f", 10,0,0) + struct.pack("<f", 0), "Bob 移动到 (10,0,0)")
a.rx()  # Alice 收到 Bob 的移动转播（不含自己）

step("4. 开火与命中（Fire → 服务器 hitscan 权威判定 → Hit）")
for i, expect in enumerate([75, 50, 25], 1):
    a.tx(7, struct.pack("<3f", 0,0,0) + struct.pack("<3f", 1,0,0), f"第{i}枪：原点朝 +x")
    a.rx()
step("4b. 第 4 枪击杀")
a.tx(7, struct.pack("<3f", 0,0,0) + struct.pack("<3f", 1,0,0))
a.rx()

step("5. 重生（服务器 3 秒定时器 → Respawn）")
print("  ...等待服务器重生定时器...")
a.rx(timeout=4.0)

step("6. 心跳与 RTT（Ping → Pong 原样带回时间戳）")
a.tx(10, struct.pack("<Q", 1724720000123), "客户端单调时钟")
a.rx()

step("7. 断线联动（Bob 掉线 → Alice 收 PlayerLeft + GameOver）")
b.s.close()
print("  Bob 的 socket 已关闭 → 服务器级联：连接清理→退房→仅剩一人判胜")
a.rx(timeout=3.0); a.rx(timeout=3.0)

print("\n演示完成")
