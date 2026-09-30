#!/usr/bin/env python3
# Sightline 压测客户端（asyncio，与服务器同机 loopback）
# 模式：登录 → 进房(自动两两配对) → 周期 Move（喂心跳+制造负载） → 周期 Ping 测 RTT
# 用法：bench_client.py --port 8899 --n 1000 --hz 5 --duration 30 [--ramp 200]
import argparse, asyncio, struct, time, random

def frame(mid, payload): return struct.pack("<HH", len(payload), mid) + payload

async def run(args):
    rtts, ok, fail = [], 0, 0
    sent = recv_msgs = 0
    stop_at = time.monotonic() + args.duration
    rng = random.Random()

    async def one(i):
        nonlocal ok, fail, sent, recv_msgs
        try:
            r, w = await asyncio.open_connection(args.host, args.port)
        except Exception:
            fail += 1
            return
        ok += 1
        buf = bytearray()
        # 登录
        name = f"b{i}".encode()
        p = struct.pack("<H", len(name)) + name
        w.write(frame(1, p))
        await w.drain()
        # 进房（两两自动开战）
        w.write(frame(3, b""))
        await w.drain()

        pos = [rng.uniform(-15, 15), 0.0, rng.uniform(-15, 15)]
        yaw = 0.0
        period = 1.0 / args.hz if args.hz > 0 else 1.0
        next_t = time.monotonic() + rng.uniform(0, period)
        next_ping = time.monotonic() + 1.0
        ping_t0 = {}

        try:
            while time.monotonic() < stop_at:
                now = time.monotonic()
                if now >= next_t:
                    pos[0] += rng.uniform(-0.5, 0.5); pos[2] += rng.uniform(-0.5, 0.5)
                    w.write(frame(5, struct.pack("<3f", *pos) + struct.pack("<f", yaw)))
                    sent += 1
                    next_t += period
                if now >= next_ping:      # Ping 同时兼作心跳喂狗（任意消息都会重置超时）
                    t0 = time.monotonic_ns() // 1000
                    ping_t0[t0 & 0xFFFFFFFFFFFF] = now
                    w.write(frame(10, struct.pack("<Q", t0)))
                    next_ping = now + 2.0
                await w.drain()
                # 非阻塞吸收下行（广播/回包）
                try:
                    data = await asyncio.wait_for(r.read(65536), timeout=0.002)
                    if data:
                        buf += data
                        while len(buf) >= 4:
                            ln, mid = struct.unpack_from("<HH", buf, 0)
                            if len(buf) < 4 + ln: break
                            recv_msgs += 1
                            if mid == 11:            # Pong → RTT
                                t0 = struct.unpack_from("<Q", buf, 4)[0]
                                s = ping_t0.pop(t0 & 0xFFFFFFFFFFFF, None)
                                if s is not None:
                                    rtts.append((time.monotonic() - s) * 1000)
                            del buf[:4 + ln]
                except asyncio.TimeoutError:
                    pass
        except Exception:
            pass
        finally:
            try:
                w.close()
            except Exception:
                pass

    # 爬坡建立连接（每秒 ramp 个）
    tasks = []
    total_ramp = max(1.0, args.n / args.ramp)
    for i in range(args.n):
        tasks.append(asyncio.create_task(one(i)))
        if args.ramp and (i + 1) % args.ramp == 0:
            await asyncio.sleep(min(1.0, total_ramp / (args.n / args.ramp)))
    await asyncio.gather(*tasks)

    rtts.sort()
    def pct(q): return rtts[min(len(rtts) - 1, int(len(rtts) * q))] if rtts else -1
    print(f"[bench] conns ok/fail={ok}/{fail} up_msgs={sent} down_msgs={recv_msgs} "
          f"rtt_ms p50={pct(0.5):.1f} p90={pct(0.9):.1f} p99={pct(0.99):.1f} max={rtts[-1] if rtts else -1:.1f} "
          f"up_rate={sent/max(1,args.duration):.0f}/s down_rate={recv_msgs/max(1,args.duration):.0f}/s")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--n", type=int, default=500)
    ap.add_argument("--hz", type=float, default=2.0)
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--ramp", type=int, default=200)
    a = ap.parse_args()
    asyncio.run(run(a))
