# smoke_match_cycle.py —— ep07 端到端回归：同一服务进程连开两局（人机在场），逐局核对三条
# 回归锁（2026-11-03 联调三症状）：
#   ① 第 2 局起开火无判定回包 / 开局即结算（终局字段跨局泄漏）
#   ② 人机首报叠在房主出生点（名册值传递·spawnIndex 全 0）
#   ③ 人机开火/击杀不广播 → 服务器判死但客户端不知情（死亡冻结回拉的前提）
# 前置：服务器已启动（地图合同 assets/maps/NewMap.json v2·spawnPoints≥2）
# 用法：python3 scripts/smoke_match_cycle.py [port=18888]
# ⚠ 会建房/加人机/开局/离房两轮——MatchService 单局常驻，勿对正在联调的服务器跑（会顶掉进行中的对局）
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18888
# STAND=房主锚点 spawnPoints[0] 旁 42cm——开局锚定握手（首条上报须落锚点 100cm 内）放行范围内
STAND = (30.0, -30.0, 90.0)
S0 = (0.0, 0.0)                 # spawnPoints[0]：房主出生点（当前关卡合同）
S1 = (820.0, -820.0)            # spawnPoints[1]

WELCOME, LOGIN, LOGIN_RESULT = 1, 4, 5
CREATE_ROOM, CREATE_RESULT, LEAVE_ROOM = 6, 7, 10
START_MATCH, MATCH_START, ENTITY_STATE = 14, 15, 17
PLAYER_INPUT, FIRE, FIRE_RESULT = 19, 20, 21
KILL, MATCH_END, BOT_ADD = 23, 25, 26


class Conn:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.buf = b""

    def send(self, msgid, payload=b""):
        self.sock.sendall(struct.pack("<HH", 2 + len(payload), msgid) + payload)

    def pump(self, timeout):
        frames = []
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(65536)
            if chunk:
                self.buf += chunk
        except (socket.timeout, BlockingIOError):
            pass
        while len(self.buf) >= 4:
            (ln,) = struct.unpack("<H", self.buf[:2])
            if len(self.buf) < 2 + ln:
                break
            (msgid,) = struct.unpack("<H", self.buf[2:4])
            frames.append((msgid, self.buf[4:2 + ln]))
            self.buf = self.buf[2 + ln:]
        return frames

    def expect(self, want, timeout=2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for msgid, p in self.pump(0.05):
                if msgid == want:
                    return p
        return None


def name_bytes(s):
    b = s.encode()
    return bytes([len(b)]) + b


def parse_roster(p):
    n = struct.unpack("<I", p[:4])[0]
    off, out = 4, []
    for _ in range(n):
        pid = struct.unpack("<I", p[off:off + 4])[0]
        off += 4
        nl = p[off]
        nm = p[off + 1:off + 1 + nl].decode()
        off += 1 + nl
        out.append((pid, nm, p[off]))
        off += 1
    return out


def parse_entities(p):
    n = struct.unpack("<H", p[:2])[0]
    out = {}
    for i in range(n):
        o = 2 + i * 29
        pid, x, y, z, _yaw, _pitch, flags, _ack = struct.unpack("<I5fBI", p[o:o + 29])
        out[pid] = (round(x), round(y), flags)
    return out


fails = 0


def check(label, ok):
    global fails
    print(("    PASS " if ok else "    FAIL ") + label)
    if not ok:
        fails += 1


c = Conn()
assert c.expect(WELCOME) is not None, "no WELCOME"
c.send(LOGIN, name_bytes("probeH"))
assert c.expect(LOGIN_RESULT) is not None, "no LOGIN_RESULT"

for r in (1, 2):
    c.send(CREATE_ROOM, name_bytes(f"cycle{r}"))
    cr = c.expect(CREATE_RESULT)
    rid = struct.unpack("<I", cr[1:5])[0]
    c.send(BOT_ADD, bytes([1]))
    time.sleep(0.2)
    c.pump(0.05)
    c.send(START_MATCH)
    roster = parse_roster(c.expect(MATCH_START))
    me = next(x[0] for x in roster if x[1] == "probeH")
    bot = next(x[0] for x in roster if x[1].startswith("Bot"))   # Bot1/Bot2…（房间内序号命名）
    for pid, nm, _ in roster:
        if nm.startswith("Bot"):
            print(f"    名册回显 name={nm!r} 字节={nm.encode('utf-8', 'replace').hex()} 长度={len(nm)}")

    t0 = time.time()
    seq, fired, nxt = 0, False, t0
    self_result = premature_end = bot_first = dead_flag_at = kill_at = None
    bot_shots = 0
    while time.time() - t0 < 4.0:
        now = time.time()
        if now >= nxt:
            seq += 1
            c.send(PLAYER_INPUT, struct.pack("<I3f2fB", seq, *STAND, 133.0, 0.0, 0))
            nxt += 1 / 30
        if not fired and now - t0 >= 0.25:     # 早开一枪（人机反应+4 连中之前）
            c.send(FIRE)
            fired = True
        for msgid, p in c.pump(0.005):
            el = round(time.time() - t0, 2)
            if msgid == FIRE_RESULT:
                shooter, res, victim = struct.unpack("<IBI", p[:9])
                if shooter == me and self_result is None:
                    self_result = (el, "hit" if res else "miss")
                if shooter == bot:
                    bot_shots += 1
            elif msgid == KILL:
                victim, killer = struct.unpack("<II", p[:8])
                if victim == me and kill_at is None:
                    kill_at = el
            elif msgid == MATCH_END and premature_end is None:
                premature_end = el
            elif msgid == ENTITY_STATE:
                ents = parse_entities(p)
                if bot in ents and bot_first is None:
                    bot_first = ents[bot][:2]
                if me in ents and ents[me][2] & 0x02 and dead_flag_at is None:
                    dead_flag_at = el

    print(f"—— 第 {r} 局（房 {rid}·我 pid={me}·人机 pid={bot}）——")
    print(f"    观测：自己开火回包={self_result} 人机首个快照位={bot_first} 过早结算={premature_end}s "
          f"人机开火 21 号={bot_shots} 条 服务器判我死={dead_flag_at}s 收到击杀我 23 号={kill_at}s")
    check("① 自己开火必有 21 号判定回包", self_result is not None)
    check("① 开局 4s 内不得收到 25 号结算", premature_end is None)
    check("② 人机首报在 spawnPoints[1]（名册序 1·不叠房主出生点）",
          bot_first is not None and (abs(bot_first[0] - S1[0]) + abs(bot_first[1] - S1[1])) < 150)
    check("③ 人机开火必须广播 21 号", bot_shots > 0)
    check("③ 服务器判我死亡时必须收到 23 号", dead_flag_at is None or kill_at is not None)

    c.send(LEAVE_ROOM)                          # 只剩人机 → 房间解散 → 驱动器自愈 AbortMatch
    time.sleep(0.5)
    c.pump(0.05)

print(f"RESULT fails={fails}")
sys.exit(1 if fails else 0)
