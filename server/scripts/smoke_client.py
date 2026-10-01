#!/usr/bin/env python3
"""第 1 期协议裸客户端验证：连接 → WELCOME → 心跳×5 → ACK/ping
不依赖 UE，独立验证服务器协议实现（定稿图心跳时序图的等价物）"""
import socket, struct, time, sys

HOST, PORT = "127.0.0.1", 8888

def recv_frame(sock):
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk: raise ConnectionError("对端关闭")
        hdr += chunk
    (ln,) = struct.unpack("<H", hdr[:2])
    (msgid,) = struct.unpack("<H", hdr[2:4])
    payload = b""
    while len(payload) < ln - 2:
        chunk = sock.recv(ln - 2 - len(payload))
        if not chunk: raise ConnectionError("对端关闭")
        payload += chunk
    return msgid, payload

def main():
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.settimeout(3)

    msgid, payload = recv_frame(s)
    assert msgid == 1, f"期望 WELCOME(1)，收到 {msgid}"
    print(f"WELCOME  ✅ serverTs={struct.unpack('<Q', payload)[0]}")

    for seq in range(1, 6):
        ts = int(time.time() * 1000)
        s.sendall(struct.pack("<HHIQ", 2 + 12, 2, seq, ts))
        mid, payload = recv_frame(s)
        assert mid == 3, f"期望 HEARTBEAT_ACK(3)，收到 {mid}"
        rseq, cts, sts = struct.unpack("<IQQ", payload)
        assert rseq == seq, f"seq 不匹配：发 {seq} 收 {rseq}"
        ping = int(time.time() * 1000) - cts
        print(f"ACK      ✅ seq={seq} ping={ping}ms")
        time.sleep(0.2)

    s.close()
    print("端到端验证 PASS（连接/WELCOME/心跳/seq 匹配/回包）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
