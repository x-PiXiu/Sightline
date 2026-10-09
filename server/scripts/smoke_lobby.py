#!/usr/bin/env python3
"""smoke_lobby.py —— 第 2 期双连接全流程验收（裸客户端，服务器外黑盒）

流程 = 设计草案 §6 时序 + 顶号：
  A 建房 → B 进房（列表/房内推送逐帧核对）→ A 离开（房主转移 beta）→ B 离开（解散）
  → 第三连接顶号（alpha 再登录 → 旧连接收 KICKED(1) 并被断开）
用法：python3 smoke_lobby.py [host] [port]
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8888

S2C = {"WELCOME": 1, "HEARTBEAT_ACK": 3, "LOGIN_RESULT": 5, "CREATE_RESULT": 7,
       "JOIN_RESULT": 9, "ROOM_LIST": 11, "ROOM_STATE": 12, "KICKED": 13}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "JOIN_ROOM": 8, "LEAVE_ROOM": 10}

g_pass, g_fail = 0, 0


def check(cond, label):
    global g_pass, g_fail
    if cond:
        g_pass += 1
        print(f"  ✓ {label}")
    else:
        g_fail += 1
        print(f"  ✗ {label}")


class Conn:
    def __init__(self, name):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""

    def send(self, msgid, payload=b""):
        self.sock.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)

    def login(self, name):
        self.send(C2S["LOGIN"], bytes([len(name.encode())]) + name.encode())

    def create(self, room):
        self.send(C2S["CREATE_ROOM"], bytes([len(room.encode())]) + room.encode())

    def join(self, rid):
        self.send(C2S["JOIN_ROOM"], struct.pack("<I", rid))

    def leave(self):
        self.send(C2S["LEAVE_ROOM"])

    def recv_frame(self, timeout=3.0):
        """收一条完整帧；超时返回 None"""
        deadline = time.time() + timeout
        while True:
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

    def expect(self, msgname, timeout=3.0):
        """循环收帧直到等到目标消息（旁路帧打印出来），返回 payload 或 None"""
        want = S2C[msgname]
        while True:
            got = self.recv_frame(timeout)
            if got is None:
                check(False, f"{self.name} 等待 {msgname} —— 超时")
                return None
            msgid, payload = got
            if msgid == ("CLOSED" if isinstance(msgid, str) else msgid) and msgid == "CLOSED":
                check(False, f"{self.name} 等待 {msgname} —— 连接被关闭")
                return None
            if msgid == want:
                check(True, f"{self.name} ← {msgname}")
                return payload
            print(f"    （旁路 {self.name}: msgid={msgid} len={len(payload)}）")

    def closed(self, timeout=3.0):
        """等待连接被服务器关闭"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = self.recv_frame(timeout=max(0.05, deadline - time.time()))
            if got is None:
                continue
            msgid, payload = got
            if msgid == "CLOSED":
                check(True, f"{self.name} 连接已被服务器关闭")
                return True
            if msgid == S2C["KICKED"]:
                check(True, f"{self.name} ← KICKED 码={payload[0]}")
        return False


def s1b(payload):            # 结果码
    return payload[0] if payload else -1


def s1(payload, off):        # 带长度前缀的字段
    n = payload[off]
    return payload[off + 1:off + 1 + n].decode(), off + 1 + n


def parse_list(p):
    (n,) = struct.unpack("<H", p[:2])
    off, rooms = 2, []
    for _ in range(n):
        rid = struct.unpack("<I", p[off:off + 4])[0]
        cnt = p[off + 4]
        name, off = s1b_at(p, off + 5)
        host, off = s1b_at(p, off)
        rooms.append((rid, cnt, name, host))
    return rooms


def s1b_at(p, off):
    n = p[off]
    return p[off + 1:off + 1 + n].decode(), off + 1 + n


def parse_state(p):
    rid = struct.unpack("<I", p[:4])[0]
    host, off = s1b_at(p, 4)
    n = p[off]
    off += 1
    members = []
    for _ in range(n):
        m, off = s1b_at(p, off)
        members.append(m)
    return rid, host, members


def main():
    print(f"══ 第 2 期 smoke：{HOST}:{PORT} ══")

    # A 建房者
    a = Conn("A")
    check(a.expect("WELCOME") is not None, "A ← WELCOME")
    a.login("alpha")
    p = a.expect("LOGIN_RESULT")
    check(p and p[0] == 0, "A 登录成功")
    p = a.expect("ROOM_LIST")
    check(parse_list(p) == [], "A 首次列表为空")

    # B 进房者
    b = Conn("B")
    check(b.expect("WELCOME") is not None, "B ← WELCOME")
    b.login("beta")
    p = b.expect("LOGIN_RESULT")
    check(p and p[0] == 0, "B 登录成功")
    p = b.expect("ROOM_LIST")
    check(parse_list(p) == [], "B 首次列表为空")
    p = a.expect("ROOM_LIST")
    check(parse_list(p) == [], "A 旁路列表清空（B 登录的推送）")

    # A 建房：A 在房内 → 收 ROOM_STATE；B 在大厅 → 收 ROOM_LIST
    a.create("第一房")
    p = a.expect("CREATE_RESULT")
    rid = struct.unpack("<I", p[1:5])[0]
    check(p[0] == 0 and rid == 1, f"A 建房成功 roomId={rid}")
    p = a.expect("ROOM_STATE")
    check(parse_state(p) == (1, "alpha", ["alpha"]), "A 收房内状态（自己是房主）")
    p = b.expect("ROOM_LIST")
    check(parse_list(p) == [(1, 1, "第一房", "alpha")], "B 列表实时刷新（A 建房可见）")

    # B 进房：双方收 ROOM_STATE；大厅无订阅者 → 无 ROOM_LIST
    b.join(rid)
    p = b.expect("JOIN_RESULT")
    check(p and p[0] == 0 and p[5] == 2, "B 进房成功（2 人）")
    p = a.expect("ROOM_STATE")
    check(parse_state(p) == (1, "alpha", ["alpha", "beta"]), "A 收房内状态（beta 加入）")
    p = b.expect("ROOM_STATE")
    check(parse_state(p) == (1, "alpha", ["alpha", "beta"]), "B 收房内状态")

    # A 离开 → 房主转移 beta
    a.leave()
    p = b.expect("ROOM_STATE")
    check(parse_state(p) == (1, "beta", ["beta"]), "B 收房内状态（房主转移 beta）")
    p = a.expect("ROOM_LIST")
    check(parse_list(p) == [(1, 1, "第一房", "beta")], "A 回大厅，列表房主已更新")
    # B 仍在房中（非大厅订阅者）→ 不收 ROOM_LIST，其信息载体是上面的 ROOM_STATE

    # B 离开 → 房间解散
    b.leave()
    p = b.expect("ROOM_LIST")
    check(parse_list(p) == [], "B 列表清空（房间解散）")
    p = a.expect("ROOM_LIST")
    check(parse_list(p) == [], "A 列表清空")

    # 顶号：新连接再用 alpha 登录 → 旧 A 收 KICKED(1) 并被关闭
    a2 = Conn("A2")
    check(a2.expect("WELCOME") is not None, "A2 ← WELCOME")
    a2.login("alpha")
    p = a2.expect("LOGIN_RESULT")
    check(p and p[0] == 0, "A2 顶号登录成功")
    k = a.expect("KICKED")
    check(k and k[0] == 1, "旧 A ← KICKED(1 顶号)")
    check(a.closed(), "旧 A 连接已被服务器关闭")

    print(f"══ smoke 结果：{g_pass} 通过 / {g_fail} 失败 ══")
    sys.exit(1 if g_fail else 0)


if __name__ == "__main__":
    main()
