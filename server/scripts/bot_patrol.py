#!/usr/bin/env python3
"""bot_patrol.py —— ep06 KPI 移动靶机器人（单人采集用）

角色：登录 → 建房（打印房间号）→ 等待真人加入 → 开局 → 沿"射手→中心"纵深轴
±350uu 正对射手跑近跑远（400uu/s 三角波·全程 7m，30Hz 上报）。真人射手**跟踪瞄准**
（准星跟着靶）按住开火——回溯关=追着 300ms 残影打（大量 miss），回溯开=指哪打哪；
两轮各 30 发，命中率差 = 延迟补偿价值。路线对齐地图几何（assets/maps/NewMap.json）。

用法：python3 bot_patrol.py [host] [port] [时长秒，默认 180]
退出：Ctrl+C 或时长到；120s 等不到第二人不开局退出。
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

# 巡逻参数（对齐地图几何 assets/maps/NewMap.json：地板 ±2000²，长墙 X[1482,1532]×Y[-983,-571]）
# 纵深轴 = 射手出生点(PlayerStart_1 实测 442,-474) → 巡逻中心方向：靶正对射手跑近跑远
# （跟踪瞄准=经典追枪演示；KPI 协议改"跟踪瞄准按住开火"）。最远点 (1354,-994) 距墙西面 128cm 不穿模。
import math
SHOOTER = (442.0, -474.0)          # PlayerStart_1（ep05 判定链日志实测射出位）
CX, CY  = 1050.0, -820.0           # 巡逻中心=首报落点（首报即真值，机器人自选）
AMP     = 350.0                    # 半程（全程 7m）
SPEED   = 400.0                    # uu/s（贴合角色 MaxWalkSpeed）
_dxx, _dyy = CX - SHOOTER[0], CY - SHOOTER[1]
_L = math.hypot(_dxx, _dyy)
AX, AY = _dxx / _L, _dyy / _L      # 纵深单位轴


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
    # 等真人加入（ROOM_STATE [4B RoomId][1B 房主名长][房主名][1B 人数]——人数在房主名后，
    # 不能按定长 offset 读：v1 把 offset4 的房主名长当人数，永不命中→白等 120s 超时开局）
    deadline = time.time() + 120
    joined = False
    while time.time() < deadline:
        got = c.recv_frame(1.0)
        if got and got[0] == S2C["ROOM_STATE"]:
            body = got[1]
            host_len = body[4]
            members = body[5 + host_len]
            if members >= 2:
                joined = True
                break
    if not joined:
        print("⚠ 120s 内未等到第二人进房——不开局退出（防单人名册哑局），请重跑脚本后先进房")
        return
    c.send(C2S["START_MATCH"])
    assert c.expect("MATCH_START") is not None
    print(f"开局完成——纵深巡逻开始（沿射手视线轴 ±{AMP:.0f}uu · {SPEED:.0f}uu/s · 单程 {2*AMP/SPEED:.1f}s）")

    seq = 0
    t0 = time.monotonic()
    nxt = t0
    while time.monotonic() - t0 < DUR:
        el = time.monotonic() - t0
        # 三角波：-AMP..+AMP 恒速往返（off<0 靠近射手 / >0 远离）
        ph = el % (2 * AMP / SPEED)
        off = (ph - AMP / SPEED) * SPEED if ph < AMP / SPEED else (AMP / SPEED - (ph - AMP / SPEED)) * SPEED
        seq += 1
        c.send(C2S["PLAYER_INPUT"],
               struct.pack("<I3f2fB", seq, CX + AX * off, CY + AY * off, 90.0, 0.0, 0.0, 0))
        nxt += 1.0 / 30.0
        d = nxt - time.monotonic()
        if d > 0:
            time.sleep(d)
    c.sock.close()
    print(f"完成：{seq} 条 19 号（30Hz×{DUR}s）")


if __name__ == "__main__":
    main()
