#!/usr/bin/env python3
"""bot_patrol.py —— ep06 KPI 移动靶机器人（单人采集用·无限局循环）

角色：登录 → 建房（打印房间号）→ 无限等待真人加入 → 开局 → 沿"射手→中心"纵深轴
±1000uu 正对射手跑近跑远（400uu/s 三角波·全程 20m，30Hz 上报）。真人射手**跟踪瞄准**
（准星跟着靶）按住开火——回溯关=追着 300ms 残影打（大量 miss），回溯开=指哪打哪。
一局结束（对方离开/断线）→ 自动 LEAVE 旧房→新建房→继续无限等——服务器房间无数量
上限、开局无时限（lobby.h 只有房员 ≤4/房名长度限制），可挂机跑一整天。

用法：python3 bot_patrol.py [host] [port]
退出：Ctrl+C 收摊；断线自动重连+重新登录后继续循环。
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18888

S2C = {"WELCOME": 1, "LOGIN_RESULT": 5, "CREATE_RESULT": 7, "JOIN_RESULT": 9,
       "ROOM_STATE": 12, "MATCH_START": 15, "ENTITY_LEAVE": 18}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "LEAVE_ROOM": 10, "START_MATCH": 14, "PLAYER_INPUT": 19}

# 巡逻参数（对齐地图几何 assets/maps/NewMap.json：地板 ±2000²，长墙 X[1482,1532]×Y[-983,-571]）
# 纵深轴 = 射手出生点(PlayerStart_1 实测 442,-474) → 巡逻中心方向：靶正对射手跑近跑远
# （跟踪瞄准=经典追枪演示；KPI 协议改"跟踪瞄准按住开火"）。全程 20m 机检：
# 最近点 (181,-325) 距射手 3m；最远点 (1919,-1315) 地板内；路径过墙X区[1482,1532]时
# 在墙Y南界外 83~111cm 掠过不穿模（胶囊半径 34 净空 ≥49cm）。
import math
SHOOTER = (442.0, -474.0)          # PlayerStart_1（ep05 判定链日志实测射出位）
CX, CY  = 1050.0, -820.0           # 巡逻中心=首报落点（首报即真值，机器人自选）
AMP     = 1000.0                   # 半程（全程 20m）
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


    def try_recv(self):
        """非阻塞收一帧：无数据=None / 断线=('CLOSED', b'')；攒够一帧就解一帧"""
        self.sock.settimeout(0)
        try:
            chunk = self.sock.recv(65536)
        except (socket.timeout, BlockingIOError):   # 非阻塞无数据（EWOULDBLOCK）≠断线——
            return None                              # BlockingIOError 是 OSError 子类，必须先捕（v1 把它当断线→开局秒退）
        except OSError:
            return ("CLOSED", b"")
        if not chunk:
            return ("CLOSED", b"")
        self.buf += chunk
        while len(self.buf) >= 4:
            (ln,) = struct.unpack("<H", self.buf[:2])
            if len(self.buf) < 2 + ln:
                break
            (msgid,) = struct.unpack("<H", self.buf[2:4])
            payload = self.buf[4:2 + ln]
            self.buf = self.buf[2 + ln:]
            return (msgid, payload)
        return None


def connect_and_login():
    """建连接+登录（断线重连共用）；连接失败/断线返回 None"""
    try:
        c = Conn("bot")
        if c.recv_frame()[0] != S2C["WELCOME"]:
            return None
        c.send(C2S["LOGIN"], bytes([3]) + "bot".encode())
        assert c.expect("LOGIN_RESULT") is not None
        return c
    except (OSError, AssertionError):
        return None


def play_one_round(c, round_no):
    """一整局：LEAVE 旧房→新建房→无限等人→开局→巡逻到对方离开/断线。
    返回 'over'（对方离开，连接仍活着）/ 'closed'（连接断开，需重连）。"""
    c.send(C2S["LEAVE_ROOM"])                        # 上一局的房还在（对局无终止机制，房里只剩 bot）
    time.sleep(0.2)                                  # 排空 LEAVE 期间的广播（ROOM_LIST 等）
    c.buf = b""
    c.send(C2S["CREATE_ROOM"], bytes([3]) + "bot".encode())
    p = c.expect("CREATE_RESULT")
    rid = struct.unpack("<I", p[1:5])[0]
    print(f"第 {round_no} 局建房完成：房间号 {rid}——无限等待第二人加入（Ctrl+C 收摊）")
    # 无限等真人加入（ROOM_STATE [4B RoomId][1B 房主名长][房主名][1B 人数]——人数在房主名后）
    while True:
        got = c.recv_frame(1.0)
        if got == ("CLOSED", b"") or got is None:
            return "closed" if got == ("CLOSED", b"") else "over"
        if got[0] == S2C["ROOM_STATE"]:
            body = got[1]
            members = body[5 + body[4]]
            if members >= 2:
                break
    c.send(C2S["START_MATCH"])
    assert c.expect("MATCH_START") is not None
    print(f"开局——纵深巡逻（沿射手视线轴 ±{AMP:.0f}uu · {SPEED:.0f}uu/s · 单程 {2*AMP/SPEED:.1f}s）")

    seq = 0
    t0 = time.monotonic()
    nxt = t0
    try:
        while True:                                   # 对局没结束就一直跑
            got = c.try_recv()                        # 下行监听（非阻塞）：18 号=对方离开
            if got:
                if got == ("CLOSED", b""):
                    print(f"{time.monotonic()-t0:.0f}s —— 连接断开")
                    return "closed"
                if got[0] == S2C["ENTITY_LEAVE"]:
                    print(f"{time.monotonic()-t0:.0f}s —— 对方离开，本局收场")
                    return "over"
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
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        print(f"{time.monotonic()-t0:.0f}s —— 连接异常: {e}")
        return "closed"


def main():
    round_no = 0
    c = connect_and_login()
    while c is None:
        print("服务器未就绪，3s 后重试……")
        time.sleep(3)
        c = connect_and_login()
    total = 0
    while True:                                       # 无限局循环：一局收场→自动开新局
        round_no += 1
        outcome = play_one_round(c, round_no)
        if outcome == "closed":
            c.sock.close()
            print("重连中……")
            time.sleep(2)
            c = connect_and_login()
            while c is None:
                print("服务器未就绪，3s 后重试……")
                time.sleep(3)
                c = connect_and_login()


if __name__ == "__main__":
    main()
