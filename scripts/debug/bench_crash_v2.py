#!/usr/bin/env python3
# 崩溃复现 v2：连接 → 流量(模拟压测Phase A) → 断线风暴，循环直到服务端崩溃
import asyncio, struct, sys, time

def frame(mid, payload): return struct.pack("<HH", len(payload), mid) + payload

async def one(i, port, traffic_s, closing):
    try:
        r, w = await asyncio.open_connection("127.0.0.1", port)
    except Exception:
        return None
    name = f"x{i}".encode()
    p = struct.pack("<H", len(name)) + name
    w.write(frame(1, p))
    w.write(frame(3, b""))
    await w.drain()
    pos = [0.0, 0.0, 0.0]
    end = time.monotonic() + traffic_s
    while time.monotonic() < end:
        pos[0] += 0.3
        w.write(frame(5, struct.pack("<3f", *pos) + struct.pack("<f", 0.0)))
        w.write(frame(10, struct.pack("<Q", time.monotonic_ns() // 1000)))
        try:
            await asyncio.wait_for(r.read(65536), timeout=0.01)
        except asyncio.TimeoutError:
            pass
        await w.drain()
        await asyncio.sleep(0.2)
    closing.append(w)
    return w

async def round_(n, port, traffic_s):
    closing = []
    await asyncio.gather(*[one(i, port, traffic_s, closing) for i in range(n)])
    for w in closing:               # 风暴：不读残留数据直接关 → RST
        w.close()
    await asyncio.sleep(1.5)
    # 探活
    try:
        r, w = await asyncio.open_connection("127.0.0.1", port)
        w.close()
        return True
    except Exception:
        return False

async def main():
    port = int(sys.argv[1]); n = int(sys.argv[2])
    traffic_s = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    for k in range(10):
        alive = await round_(n, port, traffic_s)
        print(f"round {k}: server {'ALIVE' if alive else 'DEAD'}", flush=True)
        if not alive:
            return

asyncio.run(main())
