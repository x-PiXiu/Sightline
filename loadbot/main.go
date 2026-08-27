// Sightline 协议压测 bot（Go，goroutine-per-conn）
//
// 与 Python 版（scripts/bench/bench_client.py）同协议、同行为：
//   登录 → 进房（两两自动开战）→ 按 hz 上报 Move（兼作心跳喂狗）→ 周期 Ping 测 RTT
// Go 的优势：单进程数万并发连接、真实并发调度（Python asyncio 是单线程的）。
//
// 用法：
//   ./loadbot -host 127.0.0.1 -port 8888 -n 10000 -hz 2 -dur 30 -ramp 2000
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"math/rand"
	"net"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

var (
	host    = flag.String("host", "127.0.0.1", "服务器地址")
	port    = flag.Int("port", 8888, "服务器端口")
	num     = flag.Int("n", 1000, "并发连接数")
	hz      = flag.Float64("hz", 2, "每连接 Move 上报频率")
	dur     = flag.Int("dur", 30, "压测时长(秒)")
	ramp    = flag.Int("ramp", 500, "每秒新建连接数")
	pingSec = flag.Int("ping", 2, "Ping 间隔(秒)")
)

// counters 聚合计数（atomic 松弛语义，压测统计足够）
type counters struct {
	connOK, connFail, up, down uint64
}

// frame 编码 [2B len][2B msgid][payload]（小端，与服务器契约一致）
func frame(msgid uint16, payload []byte) []byte {
	out := make([]byte, 4+len(payload))
	binary.LittleEndian.PutUint16(out[0:], uint16(len(payload)))
	binary.LittleEndian.PutUint16(out[2:], msgid)
	copy(out[4:], payload)
	return out
}

func encodeMove(x, z float32) []byte {
	b := make([]byte, 16)
	binary.LittleEndian.PutUint32(b[0:], math.Float32bits(x))
	binary.LittleEndian.PutUint32(b[4:], math.Float32bits(0)) // y
	binary.LittleEndian.PutUint32(b[8:], math.Float32bits(z))
	binary.LittleEndian.PutUint32(b[12:], 0)                  // yaw
	return b
}

var rttsMu sync.Mutex
var rtts []float64

func main() {
	flag.Parse()
	addr := fmt.Sprintf("%s:%d", *host, *port)
	var c counters
	deadline := time.Now().Add(time.Duration(*dur) * time.Second)

	// 进度播报
	go func() {
		var lastUp, lastDown uint64
		for range time.Tick(5 * time.Second) {
			up, down := atomic.LoadUint64(&c.up), atomic.LoadUint64(&c.down)
			fmt.Printf("[tick] conn_ok=%d conn_fail=%d up=%d(%d/s) down=%d(%d/s)\n",
				atomic.LoadUint64(&c.connOK), atomic.LoadUint64(&c.connFail),
				up, (up-lastUp)/5, down, (down-lastDown)/5)
			lastUp, lastDown = up, down
		}
	}()

	var wg sync.WaitGroup
	start := time.Now()
	for i := 0; i < *num; i++ {
		// 爬坡：按 ramp 速率放行
		for int(float64(*ramp)*time.Since(start).Seconds()) <= i {
			time.Sleep(5 * time.Millisecond)
		}
		wg.Add(1)
		go bot(i, addr, deadline, &c, &wg)
	}
	wg.Wait()

	rttsMu.Lock()
	sorted := append([]float64(nil), rtts...)
	rttsMu.Unlock()
	sort.Float64s(sorted)
	pct := func(q float64) float64 {
		if len(sorted) == 0 {
			return -1
		}
		return sorted[int(float64(len(sorted)-1)*q)]
	}
	fmt.Printf("[loadbot] conns ok/fail=%d/%d up_msgs=%d down_msgs=%d "+
		"rtt_ms p50=%.1f p90=%.1f p99=%.1f max=%.1f "+
		"up_rate=%.0f/s down_rate=%.0f/s\n",
		atomic.LoadUint64(&c.connOK), atomic.LoadUint64(&c.connFail),
		atomic.LoadUint64(&c.up), atomic.LoadUint64(&c.down),
		pct(0.5), pct(0.9), pct(0.99), sorted[len(sorted)-1],
		float64(atomic.LoadUint64(&c.up))/float64(*dur),
		float64(atomic.LoadUint64(&c.down))/float64(*dur))
}

func bot(id int, addr string, deadline time.Time, c *counters, wg *sync.WaitGroup) {
	defer wg.Done()
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		atomic.AddUint64(&c.connFail, 1)
		return
	}
	defer conn.Close()
	if tc, ok := conn.(*net.TCPConn); ok {
		tc.SetNoDelay(true) // 与真实游戏客户端一致
	}
	atomic.AddUint64(&c.connOK, 1)

	// 登录 + 进房
	name := fmt.Sprintf("bot%d", id)
	loginPayload := make([]byte, 2+len(name))
	binary.LittleEndian.PutUint16(loginPayload[0:], uint16(len(name)))
	copy(loginPayload[2:], name)
	conn.Write(append(frame(1, loginPayload), frame(3, nil)...))

	// 读协程：拆帧计数；Pong → RTT
	pending := make(map[uint64]time.Time)
	var pmu sync.Mutex
	go func() {
		buf := make([]byte, 0, 16384)
		tmp := make([]byte, 8192)
		for {
			n, err := conn.Read(tmp)
			if err != nil {
				return
			}
			buf = append(buf, tmp[:n]...)
			for len(buf) >= 4 {
				ln := int(binary.LittleEndian.Uint16(buf[0:]))
				mid := binary.LittleEndian.Uint16(buf[2:])
				if len(buf) < 4+ln {
					break
				}
				atomic.AddUint64(&c.down, 1)
				if mid == 11 && ln >= 8 { // Pong
					t0 := binary.LittleEndian.Uint64(buf[4:])
					pmu.Lock()
					sent, ok := pending[t0]
					if ok {
						delete(pending, t0)
					}
					pmu.Unlock()
					if ok {
						rttsMu.Lock()
						rtts = append(rtts, float64(time.Since(sent).Microseconds())/1000.0)
						rttsMu.Unlock()
					}
				}
				buf = buf[4+ln:]
			}
		}
	}()

	// 写循环：Move @hz + Ping @pingSec
	r := rand.New(rand.NewSource(int64(id)))
	x, z := r.Float32()*30-15, r.Float32()*30-15
	period := time.Duration(float64(time.Second) / *hz)
	nextMove := time.Now()
	nextPing := time.Now().Add(time.Duration(*pingSec) * time.Second)

	for time.Now().Before(deadline) {
		now := time.Now()
		if !now.Before(nextMove) {
			x += r.Float32() - 0.5
			z += r.Float32() - 0.5
			if _, err := conn.Write(frame(5, encodeMove(x, z))); err != nil {
				return
			}
			atomic.AddUint64(&c.up, 1)
			nextMove = nextMove.Add(period)
			if nextMove.Before(now.Add(-2 * period)) { // 落后过多则重置（防追赶风暴）
				nextMove = now.Add(period)
			}
		}
		if !now.Before(nextPing) {
			t0 := uint64(time.Now().UnixNano() / 1000)
			pmu.Lock()
			pending[t0] = time.Now()
			pmu.Unlock()
			if _, err := conn.Write(frame(10, u64b(t0))); err != nil {
				return
			}
			nextPing = now.Add(time.Duration(*pingSec) * time.Second)
		}
		// 睡到下一个事件（避免高频空转轮询）
		sleep := nextMove.Sub(now)
		if d := nextPing.Sub(now); d < sleep {
			sleep = d
		}
		if sleep > 50*time.Millisecond {
			sleep = 50 * time.Millisecond
		}
		if sleep > 0 {
			time.Sleep(sleep)
		}
	}
}

func u64b(v uint64) []byte {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint64(b, v)
	return b
}
