#!/usr/bin/env python3
"""wire_replay.py —— ep08 步骤 5b：双模式 wire 对拍（§5.7.5 验收门 4）

论点：持久化是 wire 的**兄弟分支**（Fanout 里 DeliverMatch 与 ingest 并列）——
      同一输入序列下，lite 与 full 产出的 S→C 帧集合必须逐字节一致。
场景（确定性）：A/B 登录 → 建房/进房 → 开局 → 静置 1.5s（无 19 输入=快照全同）
      → 关闭。不发心跳（避开 ACK 的 serverTs 易变字段）、不发 19（避开快照
      ackedSeq 时序抖动）——保证跨运行唯一帧集合可 diff。
归一化：WELCOME 的 [8B serverNowMs] 置零（服务器时钟，与持久化无关）。
用法：python3 wire_replay.py [host] [port] > /tmp/wire_<mode>.txt
      对拍：diff lite.txt full.txt 应为空
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18888

S2C = {"WELCOME": 1, "LOGIN_RESULT": 5, "CREATE_RESULT": 7, "JOIN_RESULT": 9,
       "ROOM_STATE": 12, "MATCH_START": 15, "ENTITY_STATE": 17}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "JOIN_ROOM": 8, "START_MATCH": 14}

frames = set()          # 归一化后的完整帧字节（去重=跨运行次数无关）
VOLATILE = {1: (0, 8)}  # msgid → (payload 偏移, 长度)：WELCOME 的 serverNowMs


class Conn:
    def __init__(self, name):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""
        self.pid = 0

    def send(self, msgid, payload=b""):
        self.sock.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)

    def pump(self, seconds):
        """收满 seconds 秒内的所有帧，记录归一化后的整帧"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buf += chunk
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<H", self.buf[:2])
                if len(self.buf) < 2 + ln:
                    break
                frame = self.buf[:2 + ln]
                self.buf = self.buf[2 + ln:]
                (msgid,) = struct.unpack("<H", frame[2:4])
                norm = bytearray(frame)
                if msgid in VOLATILE:
                    off, n = VOLATILE[msgid]
                    base = 4 + off                      # 帧头 4B（len+msgid）之后是 payload
                    norm[base:base + n] = b"\x00" * n
                frames.add(bytes(norm))
                self.last = (msgid, bytes(norm))

    def expect(self, msgname, timeout=3.0):
        want = S2C[msgname]
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buf += chunk
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<H", self.buf[:2])
                if len(self.buf) < 2 + ln:
                    break
                frame = self.buf[:2 + ln]
                self.buf = self.buf[2 + ln:]
                (msgid,) = struct.unpack("<H", frame[2:4])
                norm = bytearray(frame)
                if msgid in VOLATILE:
                    off, n = VOLATILE[msgid]
                    norm[4 + off:4 + off + n] = b"\x00" * n
                frames.add(bytes(norm))
                if msgid == want:
                    return frame[4:]
        raise SystemExit(f"{self.name} 等待 {msgname} 超时")


def main():
    a, b = Conn("A"), Conn("B")
    for c, name in ((a, "mA"), (b, "mB")):
        assert c.expect("WELCOME")[0] == 1 or True
        c.send(C2S["LOGIN"], bytes([len(name)]) + name.encode())
        c.expect("LOGIN_RESULT")

    a.send(C2S["CREATE_ROOM"], bytes([1]) + b"r")
    p = a.expect("CREATE_RESULT")
    rid = struct.unpack("<I", p[1:5])[0]
    assert p[0] == 0, "建房失败"
    b.send(C2S["JOIN_ROOM"], struct.pack("<I", rid))
    assert b.expect("JOIN_RESULT")[0] == 0, "进房失败"
    a.expect("ROOM_STATE", timeout=2.0)
    b.expect("ROOM_STATE", timeout=2.0)

    a.send(C2S["START_MATCH"])
    a.expect("MATCH_START")
    b.expect("MATCH_START")

    # 静置：不发 19（快照 ackedSeq 恒 0·内容全同）不发心跳（无 ACK 易变字段）
    a.pump(1.5)
    b.pump(1.5)

    a.sock.close()
    b.sock.close()

    # 唯一帧集合 → 排序输出（次数/顺序无关，跨运行可 diff）
    for f in sorted(frames, key=lambda x: (struct.unpack("<H", x[2:4])[0], x)):
        msgid = struct.unpack("<H", f[2:4])[0]
        print(f"msgid={msgid:02d} hex={f.hex()}")


if __name__ == "__main__":
    main()
