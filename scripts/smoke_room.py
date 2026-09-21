#!/usr/bin/env python3
# smoke_room.py —— D4 大厅六消息端到端冒烟（服务端视角）：
#   ListRooms(22→23) → CreateRoom(24→25) → JoinRoom(指定房) → RoomStart(4)
#   → QueryRecord(20→21)
# 前置：无（登录走游客路径）；用法: python3 scripts/smoke_room.py [host] [port]

import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8889

def frame(mid, payload=b""):
    return struct.pack("<HH", len(payload), mid) + payload

def recv_frame(s, timeout=3):
    s.settimeout(timeout)
    hdr = b""
    while len(hdr) < 4:
        c = s.recv(4 - len(hdr))
        if not c: return None, None
        hdr += c
    ln, mid = struct.unpack("<HH", hdr)
    p = b""
    while len(p) < ln:
        c = s.recv(ln - len(p))
        if not c: break
        p += c
    return mid, p

def guest_login(name):
    s = socket.create_connection((HOST, PORT), timeout=3)
    nb = name.encode()
    s.sendall(frame(1, struct.pack("<H", len(nb)) + nb))
    mid, p = recv_frame(s)
    assert mid == 2 and p[4] == 1, "游客登录失败 mid=%r" % mid
    return s, struct.unpack("<I", p[0:4])[0]

def drain_till(s, want, timeout=3):
    """排空中间帧（ItemSpawn 等）直到想 要的消息；返回 (mid, payload)"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        mid, p = recv_frame(s, timeout=max(0.1, deadline - time.time()))
        if mid in (want, None): return mid, p
    return None, None

print("== 1. A 游客登录 ==")
sa, pa = guest_login("RM_A"); print("   A pid=%d" % pa)

print("== 2. C2S_ListRooms(22) → S2C_RoomList(23) ==")
sa.sendall(frame(22))
mid, p = recv_frame(sa)
assert mid == 23, "期望 RoomList(23)，收到 %r" % mid
cnt = struct.unpack("<H", p[0:2])[0]
rooms = {}
for i in range(cnt):
    off = 2 + i * 7
    rid = struct.unpack("<I", p[off:off+4])[0]
    rooms[rid] = (p[off+4], p[off+5], p[off+6])   # (mode, cur, max)
print("   房间 %d 间: %s" % (cnt, rooms))

print("== 3. C2S_CreateRoom(24) → S2C_JoinAck(25) ==")
sa.sendall(frame(24, b"\x01"))
mid, p = recv_frame(sa)
assert mid == 25 and len(p) >= 6, "期望 JoinAck(25)，收到 %r" % mid
room_id = struct.unpack("<I", p[0:4])[0]
assert p[4] == 1, "JoinAck ok=0"
print("   建房成功 room_id=%d mode=%d" % (room_id, p[5]))

print("== 4. A 指定加入自己建的房 C2S_JoinRoom(3, roomId) ==")
sa.sendall(frame(3, struct.pack("<I", room_id)))
time.sleep(0.3)
sa.sendall(frame(22))                     # 服务端不主动推列表——显式查询核对手动建房人数
mid, p = recv_frame(sa)
assert mid == 23, "期望 RoomList(23)，收到 %r" % mid
cnt = struct.unpack("<H", p[0:2])[0]
mine = [p[2+i*7+5] for i in range(cnt) if struct.unpack("<I", p[2+i*7:2+i*7+4])[0] == room_id]
print("   房 %d 当前人数=%d（应为 1）" % (room_id, mine[0] if mine else -1))
assert mine and mine[0] == 1, "建房后加入未生效"

print("== 5. B 登录 + 指定加入同一房 → 双方 RoomStart(4) ==")
sb, pb = guest_login("RM_B"); print("   B pid=%d" % pb)
sb.sendall(frame(3, struct.pack("<I", room_id)))
mida, _ = recv_frame(sa); midb, _ = recv_frame(sb)
print("   A 收 mid=%s, B 收 mid=%s" % (mida, midb))
assert mida == 4 and midb == 4, "指定加入未开战"

print("== 6. C2S_QueryRecord(20, 8B accountId) → S2C_RecordList(21) ==")
#accountId = int(time.time()) & 0xFFFFFF
sa.sendall(frame(20, struct.pack("<Q", 3)))
mid, p = drain_till(sa, 21)
if mid != 21:
    mid, p = drain_till(sa, 21, 2)
assert mid == 21, "期望 RecordList(21)，收到 %r" % mid
cnt = struct.unpack("<H", p[0:2])[0]
print("   战绩 %d 条" % cnt)
for i in range(min(cnt, 3)):
    off = 2 + i * 14
    match_id = struct.unpack("<Q", p[off:off+8])[0]
    print("   [match_seq=%d mode=%d win=%d kills=%d deaths=%d]" %
          (match_id, p[off+8], p[off+9], struct.unpack("<H", p[off+10:off+12])[0],
           struct.unpack("<H", p[off+12:off+14])[0]))

sb.close(); sa.close()
print("[PASS] ListRooms/CreateRoom/JoinRoom(指定)/QueryRecord 全链路通过")
