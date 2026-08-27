#!/usr/bin/env python3
# 断线风暴复现器：N 连接建立后立即全部关闭（复现压测崩溃）
import asyncio, struct, sys

async def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8899
    conns = []
    for i in range(n):
        try:
            r, w = await asyncio.open_connection("127.0.0.1", port)
            name = f"x{i}".encode()
            p = struct.pack("<H", len(name)) + name
            w.write(struct.pack("<HH", len(p), 1) + p)
            w.write(struct.pack("<HH", 0, 3))          # join
            await w.drain()
            conns.append(w)
        except Exception as e:
            print(f"connect fail at {i}: {e}"); break
    print(f"connected {len(conns)}, closing all...")
    for w in conns:
        w.close()          # 不等待、不读下行 → 对端缓冲未读 → RST 风暴
    await asyncio.sleep(2)
    print("closed")

asyncio.run(main())
