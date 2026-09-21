#!/usr/bin/env python3
# smoke_admin.py —— GM Admin API 端到端冒烟：
#   双客户端游客登录 → JoinRoom(自动匹配) → RoomStart 同房 →
#   Admin /api/players 核对同房 → /api/kick 踢 A → A 断线 + players 核对
# 用法: python3 scripts/smoke_admin.py [host] [game_port] [admin_port] [token]

import socket, struct, sys, time, json, urllib.request

HOST  = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT  = int(sys.argv[2]) if len(sys.argv) > 2 else 8889
APORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
TOKEN = sys.argv[4] if len(sys.argv) > 4 else "sightline-dev-token"

def api(path, method="GET", body=None):
    req = urllib.request.Request("http://%s:%d%s" % (HOST, APORT, path), method=method,
                                 headers={"Authorization": "Bearer " + TOKEN})
    if body is not None:
        req.add_header("Content-Type", "application/json")
        req.data = json.dumps(body).encode()
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())

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
    payload = b""
    while len(payload) < ln:
        c = s.recv(ln - len(payload))
        if not c: break
        payload += c
    return mid, payload

def guest_login(name):
    s = socket.create_connection((HOST, PORT), timeout=3)
    nb = name.encode()
    s.sendall(frame(1, struct.pack("<H", len(nb)) + nb))          # C2S_Login 游客（无 64B 尾）
    mid, p = recv_frame(s)
    assert mid == 2, "期望 LoginAck(2)，收到 %r" % mid
    pid = struct.unpack("<I", p[0:4])[0]
    assert p[4] == 1, "登录失败"
    return s, pid

print("== 1. 双客户端游客登录 ==")
sa, pa = guest_login("GM_A")
sb, pb = guest_login("GM_B")
print("   A pid=%d, B pid=%d" % (pa, pb))
assert pa and pb and pa != pb, "pid 异常"

print("== 2. 各自 JoinRoom(自动匹配, room_id=0) ==")
sa.sendall(frame(3, struct.pack("<I", 0)))
time.sleep(0.2)
sb.sendall(frame(3, struct.pack("<I", 0)))

print("== 3. 双方都应收到 RoomStart(4)（人齐开战）==")
mida, _ = recv_frame(sa)
midb, _ = recv_frame(sb)
print("   A 收 mid=%s, B 收 mid=%s" % (mida, midb))
assert mida == 4 and midb == 4, "自动匹配失败：未开战（findJoinableRoom 回归？）"

print("== 4. Admin /api/players 核对：2 人同房 ==")
r = api("/api/players")
players = {p["player_id"]: p for p in r["players"]}
assert len(players) == 2, "在线人数=%d，期望 2" % len(players)
room_a, room_b = players[pa]["room_id"], players[pb]["room_id"]
assert room_a == room_b and room_a != 0, "不同房: A=%s B=%s" % (room_a, room_b)
print("   同在房间 %d ✓  %s" % (room_a, {pid: p["name"] for pid, p in players.items()}))

print("== 5. Admin /api/rooms 核对：房间 Playing(1) ==")
r = api("/api/rooms")
room = [x for x in r["rooms"] if x["room_id"] == room_a]
assert room and room[0]["state"] == 1 and room[0]["cur_players"] == 2, r
print("   %s ✓" % room[0])

print("== 6. Admin /api/kick 踢 A(pid=%d) ==" % pa)
r = api("/api/kick", "POST", {"player_id": pa})
assert r.get("ok"), r
time.sleep(0.5)
mid, p = None, None
deadline = time.time() + 3                     # 排空积压帧（开局 ItemSpawn 等）直到 Kick 或断开
while time.time() < deadline:
    mid, p = recv_frame(sa)
    if mid in (12, None):
        break
print("   A 收到 mid=%s（Kick=12 或连接关闭）" % mid)
assert mid in (12, None), "A 未收到 Kick 通告"
rest = api("/api/players")["players"]
assert [x for x in rest if x["player_id"] == pa] == [], "A 仍在线"
assert len(rest) == 1 and rest[0]["player_id"] == pb, "B 状态异常"
print("   A 已离线，B 仍在线 ✓")

print("== 7. /api/stats 总览 ==")
st = api("/api/stats")
assert st["connections"] == 1 and st["rooms"] == 1, st
print("   %s ✓" % st)

sb.close()
print("[PASS] 自动匹配 → Admin 观察 → Admin 踢人 全链路通过")
