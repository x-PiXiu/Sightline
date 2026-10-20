#!/usr/bin/env python3
"""smoke_match.py —— 第 4 期对局链路黑盒验收（裸客户端；契约 v4/v4.1）

流程：A 建房 → B 进房 → A 开局(14) → 双端各发 5 条 19（诚实轨迹 16.5uu/条）→
     收 17 v2 核对（v4.1 含己：n=全员、自实体在包、ackedSeq 回环到 5、权威位≈上报位）
验证点 = M3a 权威推算（诚实路径恒等）+ M3b 对账基准（自实体随包）的端到端闭环。
用法：python3 smoke_match.py [host] [port]
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18888

S2C = {"WELCOME": 1, "LOGIN_RESULT": 5, "CREATE_RESULT": 7, "JOIN_RESULT": 9,
       "ROOM_STATE": 12, "MATCH_START": 15, "ENTITY_STATE": 17}
C2S = {"LOGIN": 4, "CREATE_ROOM": 6, "JOIN_ROOM": 8, "START_MATCH": 14,
       "PLAYER_INPUT": 19}

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
            # 对局期 33ms 快照/房内状态旁路属正常，不逐条打印防刷屏
        check(False, f"{self.name} 等待 {msgname} 超时")
        return None


def parse_entities(payload):
    """17 v2：[2B n]{[4B pid][12B pos][4B yaw][4B pitch][1B flags][4B ack]}×n（29B/实体）"""
    (n,) = struct.unpack("<H", payload[:2])
    ents = []
    off = 2
    for _ in range(n):
        pid, x, y, z, yaw, pitch, flags, ack = struct.unpack_from("<I3f2fBI", payload, off)
        ents.append({"pid": pid, "x": x, "y": y, "z": z, "ack": ack})
        off += 29
    assert off == len(payload), f"帧长 {len(payload)} ≠ 2+{n}×29"
    return ents


def collect_snapshot(c, min_ack, timeout=3.0):
    """收 17 直到出现「自实体 ack ≥ min_ack」的快照（返回全部解析结果的最后一帧）"""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        got = c.recv_frame(max(0.05, deadline - time.time()))
        if got is None:
            continue
        msgid, payload = got
        if msgid == "CLOSED":
            break
        if msgid != S2C["ENTITY_STATE"]:
            continue
        ents = parse_entities(payload)
        last = ents
        if any(e["ack"] >= min_ack for e in ents if e["pid"] == c.pid):
            return ents
    return last


def main():
    a, b = Conn("A"), Conn("B")
    for c, name in ((a, "mA"), (b, "mB")):
        check(c.recv_frame()[0] == S2C["WELCOME"], f"{c.name} ← WELCOME")
        c.send(C2S["LOGIN"], bytes([len(name)]) + name.encode())
        check(c.expect("LOGIN_RESULT") is not None, f"{c.name} ← LOGIN_RESULT")

    a.send(C2S["CREATE_ROOM"], bytes([1]) + b"r")
    p = a.expect("CREATE_RESULT")
    rid = struct.unpack("<I", p[1:5])[0]
    check(p[0] == 0, f"A 建房成功 roomId={rid}")
    b.send(C2S["JOIN_ROOM"], struct.pack("<I", rid))
    check(b.expect("JOIN_RESULT")[0] == 0, "B 进房成功")
    # 房内状态推送旁路收干净（避免混入开局读帧）
    a.expect("ROOM_STATE", timeout=2.0)
    b.expect("ROOM_STATE", timeout=2.0)

    a.send(C2S["START_MATCH"])
    for c in (a, b):
        p = c.expect("MATCH_START")
        (n,) = struct.unpack("<I", p[:4])
        check(n == 2, f"{c.name} ← MATCH_START 名册 2 人")
        # 从名册认领自己的 pid（不硬编码登录序）
        off, pid_by_name = 4, {}
        for _ in range(n):
            pid = struct.unpack_from("<I", p, off)[0]
            off += 4
            nlen = p[off]
            off += 1
            nm = p[off:off + nlen].decode()
            off += nlen + 1                       # 名字 + 出生点序号
            pid_by_name[nm] = pid
        c.pid = pid_by_name["mA" if c is a else "mB"]
    check(a.pid != b.pid, f"名册 pid 认领：A={a.pid} B={b.pid}")

    # 双端各发 5 条 19（诚实轨迹：x 每条 +16.5uu < 钳制界 100uu——权威应与上报恒等）
    kSteps, kStep = 5, 16.5
    for i in range(1, kSteps + 1):
        for c in (a, b):
            c.send(C2S["PLAYER_INPUT"],
                   struct.pack("<I3f2fB", i, i * kStep, 0.0, 0.0, 0.0, 0.0, 0))
        time.sleep(0.01)
    for c in (a, b):
        ents = collect_snapshot(c, min_ack=kSteps)
        check(ents is not None, f"{c.name} 收到 17 快照")
        if not ents:
            continue
        check(len(ents) == 2, f"{c.name} 快照实体数=2（v4.1 含己）")
        selfs = [e for e in ents if e["pid"] == c.pid]
        peers = [e for e in ents if e["pid"] != c.pid]
        check(len(selfs) == 1 and len(peers) == 1, f"{c.name} 自实体在包 + 对端在包")
        if selfs:
            check(selfs[0]["ack"] == kSteps, f"{c.name} ackedSeq 回环={selfs[0]['ack']}")
            check(abs(selfs[0]["x"] - kSteps * kStep) < 1.0,
                  f"{c.name} 权威位≈上报位（x={selfs[0]['x']:.1f} vs {kSteps * kStep:.1f}，诚实路径恒等）")

    a.sock.close()
    b.sock.close()
    print(f"══ smoke_match 结果：{g_pass} 通过 / {g_fail} 失败 ══")
    sys.exit(1 if g_fail else 0)


if __name__ == "__main__":
    main()
