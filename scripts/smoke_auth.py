#!/usr/bin/env python3
# smoke_auth.py —— D2 端到端烟测：Register → Login(account+passHash) → 校验 LoginAck 加尾
# 用法：python3 smoke_auth.py [host] [port]   （默认 127.0.0.1 9999）

import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9999

def frame(mid, payload):
    return struct.pack("<HH", len(payload), mid) + payload

def recv_frame(s):
    hdr = b""
    while len(hdr) < 4:
        c = s.recv(4 - len(hdr))
        if not c: return None, None
        hdr += c
    ln, mid = struct.unpack("<HH", hdr)
    payload = b""
    while len(payload) < ln:
        c = s.recv(ln - len(payload))
        if not c: break
        payload += c
    return mid, payload

s = None
import time as _t
for _ in range(20):                            # 服务器启动需要时间，重试连接 ~10s
    try:
        s = socket.create_connection((HOST, PORT), timeout=1)
        break
    except OSError:
        _t.sleep(0.5)
if s is None:
    print("[FAIL] 无法连接 %s:%d（服务器未启动？）" % (HOST, PORT))
    sys.exit(2)
account = ("tester%06x" % (int(time.time()) & 0xFFFFFF)).encode()
ph = (b"a" * 64).hex().encode()[:64] if False else b"a" * 64

# ① 注册
s.sendall(frame(17, struct.pack("<H", len(account)) + account + ph))
mid, payload = recv_frame(s)
assert mid == 18 and len(payload) >= 2, "RegisterResult 格式异常"
ok, err = payload[0], payload[1]
print("① Register: ok=%d err=%d account=%s" % (ok, err, account.decode()))
assert ok == 1, "注册失败"

# ② 账号密码登录
s.sendall(frame(1, struct.pack("<H", len(account)) + account + ph))
mid, payload = recv_frame(s)
print("② LoginAck 调试: mid=%r payload_len=%r payload=%r" % (mid, len(payload or b""), (payload or b"").hex()))
assert mid == 2 and len(payload) >= 14, "LoginAck 格式异常"
pid      = struct.unpack("<I", payload[0:4])[0]
ok       = payload[4]
guest    = payload[5]
acct_id  = struct.unpack("<Q", payload[6:14])[0]
nlen     = struct.unpack("<H", payload[14:16])[0]
nickname = payload[16:16+nlen].decode()
w, l, k, d = struct.unpack("<HHHH", payload[16+nlen:24+nlen])
tlen     = struct.unpack("<H", payload[24+nlen:26+nlen])[0]
token    = payload[26+nlen:26+nlen+tlen].decode()
print("② Login: pid=%d ok=%d guest=%d account_id=%d nickname=%s 战绩=%d/%d/%d/%d token=%s..."
      % (pid, ok, guest, acct_id, nickname, w, l, k, d, token[:8]))
assert ok == 1 and acct_id > 0 and token, "账号登录失败"

print("[PASS] Register → Login(account+passHash) → 战绩摘要 + token 全链通过")
print("数据库核对: SELECT * FROM sightline.account WHERE account='%s';" % account.decode())
