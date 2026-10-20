#!/usr/bin/env python3
"""bot_move.py —— 对局期持续输入流复现器（诊断 tick 拖慢用，2026-10-21）

场景 = 双 PIE 联调的净输入侧：两连接进房开局后，双端以 30Hz 持续发 19 号 6 秒
（静止位置即可——复现的是服务器的帧处理节奏，不是位移语义）。
判据：服务器侧 [tick探针] 行的间隔——空闲 vs 双流负载下的差值即 tick 拖慢证据。
用法：python3 bot_move.py [host] [port]
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18888

S2C = {"WELCOME": 1, "LOGIN_RESULT": 5, "CREATE_RESULT": 7, "JOIN_RESULT": 9,
       "ROOM_STATE": 12, "MATCH_START": 15}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "JOIN_ROOM": 8, "START_MATCH": 14, "PLAYER_INPUT": 19}


class Conn:
    def __init__(self, name):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""

    def send(self, msgid, payload=b""):
        self.sock.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)

    def recv_frame(self, timeout=3.0):
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
        want = S2C[msgname]
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = self.recv_frame(max(0.05, deadline - time.time()))
            if got is None:
                break
            msgid, payload = got
            if msgid == "CLOSED":
                break
            if msgid == want:
                return payload
        return None


def main():
    a, b = Conn("A"), Conn("B")
    for c, name in ((a, "mA"), (b, "mB")):
        assert c.recv_frame()[0] == S2C["WELCOME"]
        c.send(C2S["LOGIN"], bytes([len(name)]) + name.encode())
        assert c.expect("LOGIN_RESULT") is not None

    a.send(C2S["CREATE_ROOM"], bytes([1]) + b"r")
    p = a.expect("CREATE_RESULT")
    assert p and p[0] == 0
    b.send(C2S["JOIN_ROOM"], struct.pack("<I", 1))
    assert b.expect("JOIN_RESULT")[0] == 0
    a.expect("ROOM_STATE", 2.0)
    b.expect("ROOM_STATE", 2.0)

    a.send(C2S["START_MATCH"])
    assert a.expect("MATCH_START") is not None
    assert b.expect("MATCH_START") is not None
    print("开局完成，双端 30Hz 持续输入 6s……")

    end = time.monotonic() + 6.0
    seq = 0
    nxt = time.monotonic()
    while time.monotonic() < end:
        seq += 1
        for c in (a, b):
            c.send(C2S["PLAYER_INPUT"],
                   struct.pack("<I3f2fB", seq, 100.0, 200.0, 300.0, 0.0, 0.0, 0))
        nxt += 1.0 / 30.0
        delay = nxt - time.monotonic()
        if delay > 0:
            time.sleep(delay)

    # 排空 6s 期间收到的快照（防 TCP 缓冲影响时序观测）
    for c in (a, b):
        c.sock.settimeout(0.2)
        try:
            while c.sock.recv(65536):
                pass
        except (socket.timeout, ConnectionResetError):
            pass
    a.sock.close()
    b.sock.close()
    print(f"完成：双端各发 {seq} 条 19 号（30Hz×6s），已断开")


if __name__ == "__main__":
    main()
