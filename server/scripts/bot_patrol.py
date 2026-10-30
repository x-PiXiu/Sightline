#!/usr/bin/env python3
"""bot_patrol.py —— ep06 KPI 移动靶机器人（单人采集用）

角色：登录 → 建房（打印房间号）→ 等待真人加入 → 开局 → 在出生区域沿 Y 轴
±150uu 往返巡逻（300uu/s 三角波，30Hz 上报）。真人射手瞄准路径中点按住开火，
机器人反复穿过火线——回溯关/开两轮各 30 发，命中率差 = 延迟补偿价值。

用法：python3 bot_patrol.py [host] [port] [时长秒，默认 180]
退出：Ctrl+C 或时长到；真人断线也会自动结束。
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18888
DUR = int(sys.argv[3]) if len(sys.argv) > 3 else 180

S2C = {"WELCOME": 1, "LOGIN_RESULT": 5, "CREATE_RESULT": 7, "JOIN_RESULT": 9,
       "ROOM_STATE": 12, "MATCH_START": 15}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "START_MATCH": 14, "PLAYER_INPUT": 19}

# 巡逻参数（Spawn_2 区域 · Y 轴三角波）
CX, CY, CZ = 820.0, -820.0, 90.0     # 巡逻中心（首报即真值=机器人自选落点）
AMP = 150.0                          # 半程（路径全长 300cm）
SPEED = 300.0                        # uu/s（三角波恒速）


class Conn:
    def __init__(self, name):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""

    def send(self, msgid, payload=b""):
        self.sock.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)

    def recv_frame(self, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<H", self.buf[:2])
                if len(self.buf) >= 2 + ln:
                    (msgid,) = struct.unpack("<H", self.buf[2:4])
                    payload = self.buf[4:2 + ln]
                    self.buf = self.buf[2 + ln:]
                    return msgid, payload
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return ("CLOSED", b"")
            self.buf += chunk
        return None

    def expect(self, msgname, timeout=3.0):
        want = S2C[msgname]
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = self.recv_frame(max(0.05, deadline - time.time()))
            if got is None:
                break
            msgid, payload = got
            if msgid == want:
                return payload
        return None


def main():
    c = Conn("bot")
    assert c.recv_frame()[0] == S2C["WELCOME"]
    c.send(C2S["LOGIN"], bytes([3]) + "bot".encode())
    assert c.expect("LOGIN_RESULT") is not None
    c.send(C2S["CREATE_ROOM"], bytes([3]) + "bot".encode())
    p = c.expect("CREATE_RESULT")
    rid = struct.unpack("<I", p[1:5])[0]
    print(f"机器人建房完成：房间号 {rid}——请在 PIE 登录后加入该房间，加入后本脚本自动开局")
    # 等真人加入（ROOM_STATE 成员数=2）
    deadline = time.time() + 120
    while time.time() < deadline:
        got = c.recv_frame(1.0)
        if got and got[0] == S2C["ROOM_STATE"] and got[1][4:5] == b"\x02":
            break
    else:
        pass
    c.send(C2S["START_MATCH"])
    assert c.expect("MATCH_START") is not None
    print("开局完成——巡逻开始（沿 Y ±150uu · 300uu/s）")

    seq = 0
    t0 = time.monotonic()
    nxt = t0
    while time.monotonic() - t0 < DUR:
        el = time.monotonic() - t0
        # 三角波：-AMP..+AMP 恒速往返
        ph = el % (2 * AMP / SPEED)
        off = (ph - AMP / SPEED) * SPEED if ph < AMP / SPEED else (AMP / SPEED - (ph - AMP / SPEED)) * SPEED
        seq += 1
        c.send(C2S["PLAYER_INPUT"],
               struct.pack("<I3f2fB", seq, CX, CY + off, CZ, 0.0, 0.0, 0))
        nxt += 1.0 / 30.0
        d = nxt - time.monotonic()
        if d > 0:
            time.sleep(d)
    c.sock.close()
    print(f"完成：{seq} 条 19 号（30Hz×{DUR}s）")


if __name__ == "__main__":
    main()
