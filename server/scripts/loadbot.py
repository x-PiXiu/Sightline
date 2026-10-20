#!/usr/bin/env python3
# loadbot.py —— 主从 Reactor 基线压测（M2 正确性+粗吞吐；正式压测=第 12 期叙事）
# 用法: python3 loadbot.py [N=500] [--ramp 0.002] [--hold 3.0]
# 语义: N 个并发连接 → 逐个 LOGIN（唯一名）→ 读 LOGIN_RESULT → 保持 hold 秒（收帧不断）→ 关闭
# 指标: 成功登录数/失败数/总耗时/登录吞吐；退出码=失败数>0 则 1
import asyncio, sys, time
from collections import Counter

HOST = "127.0.0.1"
PORT = int(sys.argv[sys.argv.index("--port") + 1]) if "--port" in sys.argv else 18888

def frame(msgid: int, payload: bytes = b"") -> bytes:
    ln = 2 + len(payload)
    return ln.to_bytes(2, "little") + msgid.to_bytes(2, "little") + payload

async def read_exact(r, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = await r.read(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf

async def one(i: int, ramp: float, hold: float, ok: list, bad: list):
    await asyncio.sleep(i * ramp)
    try:
        r, w = await asyncio.open_connection(HOST, PORT)
        await read_exact(r, 12)                                   # WELCOME 全帧 [2B len][2B msgid][8B ts]——漏帧头=整流错位死锁
        name = f"bot{i:05d}".encode()
        w.write(frame(4, bytes([len(name)]) + name))             # LOGIN(4)
        await w.drain()
        hdr = await read_exact(r, 4)
        ln = int.from_bytes(hdr[:2], "little")
        body = await read_exact(r, ln - 2)
        if body[0] != 0:
            bad.append(f"login code={body[0]}"); w.close(); return
        end = time.monotonic() + hold
        try:
            while time.monotonic() < end:
                data = await asyncio.wait_for(r.read(4096), timeout=end - time.monotonic() + 0.5)
                if not data:
                    bad.append("peer closed mid-hold")
                    return                              # 保持期内被服务端掐线 = 失败
        except asyncio.TimeoutError:
            pass                                        # 大厅静默属正常——保持到时即连接健康
        w.close()
        await w.wait_closed()
        ok.append(i)
    except Exception as e:
        bad.append(f"{type(e).__name__}:{e}")

async def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    ramp = float(sys.argv[sys.argv.index("--ramp") + 1]) if "--ramp" in sys.argv else 0.002
    hold = float(sys.argv[sys.argv.index("--hold") + 1]) if "--hold" in sys.argv else 3.0
    ok, bad = [], []
    t0 = time.monotonic()
    await asyncio.gather(*[one(i, ramp, hold, ok, bad) for i in range(n)])
    dt = time.monotonic() - t0
    print(f"loadbot: N={n} ramp={ramp}s hold={hold}s")
    print(f"  成功={len(ok)} 失败={len(bad)} 总耗时={dt:.1f}s 建连吞吐={len(ok)/max(dt,1e-9):.0f}/s")
    for reason, cnt in Counter(bad).most_common():
        sample = next(e for e in bad if e == reason)
        print(f"  ERR x{cnt}  {sample[:120]}")
    sys.exit(1 if bad else 0)

asyncio.run(main())
