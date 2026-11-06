#!/usr/bin/env python3
"""live_chain_test.py —— ep08 步骤 9 前置：C++ 服务端实机链路验证（WSL 启动实测）

覆盖三条"只有单测、没有实机跑过"的链：
  A) 换枪端到端：客户端发 28 号(weaponType=1) → 服务器 SwitchWeapon → 17 号快照 bit3~4=1
  B) bot 实机决策：26 号加 bot → 开局 → bot 位置随时间变化（Lua FSM 驱动）+ 21/23 号事件
  C) 30 号随开局广播（15 号后必有 30 号）
用法：先起服务器（任意模式），再 python3 scripts/live_chain_test.py
"""
import socket, struct, sys, time, json, os

# 合同出生点（与 assets/maps/NewMap.json spawnPoints 同源——19 号首报必须≈出生点，
# 否则 rebasePending 握手把它当"在途旧报"静默拒、实体永不激活（2026-11-07 实机验证轮实锤）
_sp = json.load(open(os.path.join(os.path.dirname(__file__), "..", "assets", "maps", "NewMap.json")))
SPAWNS = [tuple(sp["pos"]) for sp in _sp["spawnPoints"]]

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18888

C2S_LOGIN, C2S_CREATE, C2S_JOIN, C2S_START, C2S_INPUT = 4, 6, 8, 14, 19
C2S_BOT_ADD, C2S_SWITCH = 26, 28
S2C_MATCH_START, S2C_SNAPSHOT, S2C_FIRE, S2C_KILL, S2C_CONFIG = 15, 17, 21, 23, 30

frames = {}          # msgid → 计数
bot_ids, bot_samples = [], {}   # pid → [(t, x, y)]
wtype_seen = None
g_pass, g_fail = 0, 0

def check(cond, label):
    global g_pass, g_fail
    if cond: g_pass += 1; print(f"  ✓ {label}")
    else:    g_fail += 1; print(f"  ✗ {label}")

class Conn:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.buf = b""
        self.pid = 0
    def send(self, msgid, payload=b""):
        self.s.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)
    def pump(self, seconds, on_frame=None):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.s.settimeout(max(0.05, deadline - time.time()))
            try: chunk = self.s.recv(65536)
            except socket.timeout: break
            if not chunk: break
            self.buf += chunk
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<H", self.buf[:2])
                if len(self.buf) < 2 + ln: break
                frame = self.buf[:2 + ln]; self.buf = self.buf[2 + ln:]
                (mid,) = struct.unpack("<H", frame[2:4])
                frames[mid] = frames.get(mid, 0) + 1
                if on_frame: on_frame(mid, frame[4:])
    def expect(self, want, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.s.settimeout(max(0.05, deadline - time.time()))
            try: chunk = self.s.recv(65536)
            except socket.timeout: break
            if not chunk: break
            self.buf += chunk
            while len(self.buf) >= 4:
                (ln,) = struct.unpack("<H", self.buf[:2])
                if len(self.buf) < 2 + ln: break
                frame = self.buf[:2 + ln]; self.buf = self.buf[2 + ln:]
                (mid,) = struct.unpack("<H", frame[2:4])
                frames[mid] = frames.get(mid, 0) + 1
                if mid == want: return frame[4:]
        return None

def parse_snapshot(payload):
    (n,) = struct.unpack("<H", payload[:2])
    off, ents = 2, []
    for _ in range(n):
        pid, x, y, z, yaw, pitch, flags, ack = struct.unpack_from("<I3f2fBI", payload, off)
        ents.append((pid, x, y, flags)); off += 29
    return ents

c = Conn()
assert c.expect(1) is not None, "WELCOME"
c.send(C2S_LOGIN, bytes([4]) + b"live")
assert c.expect(5)[0] == 0, "登录"
c.send(C2S_CREATE, bytes([1]) + b"r")
p = c.expect(7); assert p[0] == 0, "建房"
rid = struct.unpack("<I", p[1:5])[0]

print("== A) 26 号加 bot（房主）==")
for _ in range(3):
    c.send(C2S_BOT_ADD, bytes([1]))
    time.sleep(0.1)

print("== 开局 → 15 号名册 ==")
c.send(C2S_START)
p = c.expect(15, 5.0)
check(p is not None, "15 号 MATCH_START 到达")
n = struct.unpack("<I", p[:4])[0] if p else 0
check(n == 4, f"名册 1 真人 + 3 bot（n={n}）")
off, names, spawn_ix = 4, {}, {}
for _ in range(n):
    pid = struct.unpack_from("<I", p, off)[0]; off += 4
    nl = p[off]; off += 1
    nm = p[off:off + nl].decode(); off += nl
    names[pid] = nm
    spawn_ix[pid] = p[off]; off += 1        # [1B 出生点序号]——19 号首报坐标锚
    if "bot" in nm.lower() or "人机" in nm:
        bot_ids.append(pid); bot_samples[pid] = []
self_pid = next((k for k, v in names.items() if v == "live"), 0)
check(self_pid != 0, f"自 pid 认领={self_pid}")

print("== C) 30 号随开局广播 ==")
c.pump(1.0)
check(frames.get(S2C_CONFIG, 0) >= 1, f"30 号已广播（收到 {frames.get(S2C_CONFIG, 0)} 次）")

print("== A) 28 号换枪端到端 ==")
sx, sy, sz = SPAWNS[spawn_ix.get(self_pid, 0)]          # 自己出生点（合同点）
c.send(C2S_INPUT, struct.pack("<I3f2fB", 1, sx, sy, sz, 0.0, 0.0, 0))   # 19 号首报=出生点（rebase 握手放行+bHasReport 激活）
time.sleep(0.3)
c.send(C2S_INPUT, struct.pack("<I3f2fB", 2, sx + 16.5, sy, sz, 0.0, 0.0, 0))  # 二报：出生点旁诚实步进
c.send(C2S_SWITCH, bytes([1]))      # weaponType=1 手枪
deadline = time.time() + 3.0
wtype_seen = None
while time.time() < deadline and wtype_seen is None:
    c.pump(0.5, lambda mid, pl: None)
    # 重收缓冲里的快照——直接再 pump 一段并解析
    def look(mid, pl):
        global wtype_seen
        if mid == S2C_SNAPSHOT:
            for pid, x, y, fl in parse_snapshot(pl):
                if pid == self_pid:
                    wtype_seen = (fl >> 3) & 0x03
    c.pump(0.5, look)
check(wtype_seen == 1, f"17 号 bit3~4 回读 weaponType=1（实际={wtype_seen}）")
c.send(C2S_SWITCH, bytes([0]))      # 换回步枪

print("== B) bot 实机决策（静置 12s 采样）==")
self_seen = [0]   # 自实体入快照次数（bHasReport 探针——19 号未采纳则恒 0）
def sample(mid, pl):
    if mid == S2C_SNAPSHOT:
        for pid, x, y, fl in parse_snapshot(pl):
            if pid == self_pid: self_seen[0] += 1
            if pid in bot_samples:
                bot_samples[pid].append((time.time(), x, y))
    elif mid in (S2C_FIRE, S2C_KILL):
        pass  # 计数已在 frames
c.pump(12.0, sample)
for pid in bot_ids:
    pts = bot_samples[pid]
    moved = len(pts) >= 2 and max(
        ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 for a, b in zip(pts, pts[1:])) > 20.0
    check(moved, f"bot[{names[pid]}] 位置随时间变化（采样 {len(pts)} 帧·位移>20cm）")
check(frames.get(S2C_FIRE, 0) >= 1, f"21 号判定事件（bot 交战·{frames.get(S2C_FIRE, 0)} 次）")
check(frames.get(S2C_KILL, 0) >= 1, f"23 号击杀播报（{frames.get(S2C_KILL, 0)} 次）")
check(self_seen[0] > 0, f"自实体在快照（19 号激活探针·{self_seen[0]} 次）")

c.s.close()
print(f"══ live_chain 结果：{g_pass} 通过 / {g_fail} 失败 ══")
sys.exit(1 if g_fail else 0)
