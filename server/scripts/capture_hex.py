# -*- coding: utf-8 -*-
"""一次性抓包脚本：连接 8888，打印 WELCOME/HEARTBEAT/ACK 每帧的 msgid 与原始字节 hex（S7 动画 FACT 用）"""
import socket, struct, time, sys

s = socket.create_connection(("127.0.0.1", 8888), timeout=3)
s.settimeout(2.0)

def send_frame(msgid, payload):
    frame = struct.pack("<HH", 2 + len(payload), msgid) + payload
    s.sendall(frame)
    print(f"C->S msgid={msgid} total={len(frame)}B hex={frame.hex(' ')}")

def recv_frames():
    buf = b""
    end = time.time() + 1.5
    while time.time() < end:
        try:
            d = s.recv(4096)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
        while len(buf) >= 4:
            ln, mid = struct.unpack("<HH", buf[:4])
            if len(buf) < ln + 2:
                break
            frame = buf[:ln + 2]
            print(f"S->C msgid={mid} total={len(frame)}B hex={frame.hex(' ')}")
            buf = buf[ln + 2:]

recv_frames()                      # WELCOME
for seq in (1, 2):
    ts = int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF
    send_frame(2, struct.pack("<IQ", seq, ts))
    time.sleep(0.2)
    recv_frames()                  # ACK
s.close()
print("CAPTURE_DONE")
