// loadbot v2 —— Sightline 容量+对局全流程压测 bot（Go · goroutine-per-conn）
//
// 血统：毕设 Sightline-archived/loadbot（单进程数万并发的正确姿势）；协议对齐 ep04 契约 v4.1。
//
// 三模式：
//
//	lobby  —— 登录 + 保持（容量语义，同 Python 版）
//	match  —— 对局全流程：登录→建房/进房(4人)→开局→30Hz 输入→快照验收（含己 v4.1 + ack 回环）
//	mixed  —— 先全部对局开局完成，再铺大厅连接，最后共同保持（=「3w 并发且其中有真对局」）
//
// 关键工程点：
//
//	① 多 dst IP（127.0.0.1/127.0.0.2/…）：单 IP 临时端口 ~28k 是硬墙，按目的 IP 均分翻倍
//	② SetLinger(0) 关闭=RST：不留 TIME_WAIT，连续跑档不漏端口（服务器侧 read reset 为正常清算）
//	③ 分阶段：对局先开局完毕，大厅后铺——避开「房间列表全量推送×大厅连接数」的 O(N²) 广播
//	④ 对局健康判据=快照速率（应≈30/s）+ ack 滞后（sent−lastAck）——main 线程被打满时先掉速再饿死，数据能看见墙
//
// 用法：
//
//	loadbot -port 18889 -n 30000 -matchN 2000 -ramp 500 -hold 20
//	loadbot -port 18889 -n 0      -matchN 2000 -hold 20        # 纯对局档
//	loadbot -port 18889 -n 30000 -matchN 0    -ramp 500 -hold 25 # 纯容量档
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"net"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	port    = flag.Int("port", 18888, "服务器端口")
	ipsFlag = flag.String("ips", "127.0.0.1,127.0.0.2", "dst IP 轮转列表（逗号分隔；单 IP 临时端口 ~28k 是墙）")
	nLobby  = flag.Int("n", 1000, "大厅连接数（登录+保持）")
	matchN  = flag.Int("matchN", 0, "对局连接数（自动按 4/房 分组；须为 4 的倍数）")
	ramp    = flag.Float64("ramp", 500, "大厅建连速率（连接/秒）")
	mramp   = flag.Float64("mramp", 200, "对局建连速率（连接/秒）")
	hold    = flag.Float64("hold", 20, "大厅铺设完毕后的保持秒数（对局全程 30Hz）")
	hz      = flag.Float64("hz", 30, "对局输入频率")
	group   = flag.Int("group", 4, "每房人数（服务器上限 4）")
)

// ---------- 协议（契约 v4.1；帧 = [2B len][2B msgid][payload]，len 含 msgid） ----------
const (
	msgWelcome    = 1
	msgLogin      = 4
	msgLoginRes   = 5
	msgCreate     = 6
	msgCreateRes  = 7
	msgJoin       = 8
	msgJoinRes    = 9
	msgRoomList   = 11
	msgRoomState  = 12
	msgStart      = 14
	msgMatchStart = 15
	msgSnap       = 17
	msgInput      = 19
)

func u16(v uint16) []byte { b := make([]byte, 2); binary.LittleEndian.PutUint16(b, v); return b }
func u32(v uint32) []byte { b := make([]byte, 4); binary.LittleEndian.PutUint32(b, v); return b }

type conn struct {
	c *net.TCPConn
	r *bufio.Reader
	w *bufio.Writer
}

func dial(dst string) (*conn, error) {
	c, err := net.DialTimeout("tcp", dst, 5*time.Second)
	if err != nil {
		return nil, err
	}
	tc := c.(*net.TCPConn)
	tc.SetNoDelay(true)
	return &conn{c: tc, r: bufio.NewReaderSize(c, 65536), w: bufio.NewWriterSize(c, 4096)}, nil
}

func (c *conn) closeRST() {
	_ = c.c.SetLinger(0) // RST 关闭：不留 TIME_WAIT，压测连跑不漏端口
	_ = c.c.Close()
}

func (c *conn) readFrame(deadline time.Time) (uint16, []byte, error) {
	_ = c.c.SetReadDeadline(deadline)
	var hdr [4]byte
	if _, err := readFull(c.r, hdr[:]); err != nil {
		return 0, nil, err
	}
	ln := int(binary.LittleEndian.Uint16(hdr[0:]))
	if ln < 2 || ln > 65535 {
		return 0, nil, fmt.Errorf("bad len %d", ln)
	}
	// 帧 = [2B len][2B msgid][payload]；len 含 msgid——header 已读走 4B，body = ln-2 = payload
	body := make([]byte, ln-2)
	if _, err := readFull(c.r, body); err != nil {
		return 0, nil, err
	}
	return binary.LittleEndian.Uint16(hdr[2:4]), body, nil
}

func readFull(r *bufio.Reader, buf []byte) (int, error) {
	n := 0
	for n < len(buf) {
		m, err := r.Read(buf[n:])
		n += m
		if err != nil {
			return n, err
		}
	}
	return n, nil
}

func (c *conn) send(msgid uint16, payload []byte) error {
	var hdr [4]byte
	binary.LittleEndian.PutUint16(hdr[0:], uint16(2+len(payload)))
	binary.LittleEndian.PutUint16(hdr[2:], msgid)
	if _, err := c.w.Write(hdr[:]); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := c.w.Write(payload); err != nil {
			return err
		}
	}
	return c.w.Flush()
}

// 跳过旁路帧直到等到目标 msgid（ROOM_LIST/ROOM_STATE 推送属正常旁路）
func (c *conn) waitMsg(want uint16, deadline time.Time) ([]byte, error) {
	for {
		mid, payload, err := c.readFrame(deadline)
		if err != nil {
			return nil, fmt.Errorf("wait msg %d: %w", want, err)
		}
		if mid == want {
			return payload, nil
		}
	}
}

// ---------- 计数器 ----------
type counters struct {
	connOK, connFail             int64
	loginOK, loginFail           int64
	createOK, createFail         int64
	joinOK, joinFail             int64
	matchStartOK, matchStartFail int64
	matchDoneOK, matchDoneFail   int64
	sentInputs, snapFrames       int64
	lobbyClosedMid               int64
	snapStarved                  int64 // 对局中 >5s 无快照
}

var c counters
var errCounter sync.Map // string -> *int64

func errAdd(s string) {
	v, _ := errCounter.LoadOrStore(s, new(int64))
	atomic.AddInt64(v.(*int64), 1)
}

func errDump() {
	type kv struct {
		k string
		v int64
	}
	var out []kv
	errCounter.Range(func(k, v any) bool {
		out = append(out, kv{k.(string), atomic.LoadInt64(v.(*int64))})
		return true
	})
	sort.Slice(out, func(i, j int) bool { return out[i].v > out[j].v })
	for i, e := range out {
		if i >= 12 {
			fmt.Printf("  … %d 种错误已省略\n", len(out)-12)
			break
		}
		s := e.k
		if len(s) > 140 {
			s = s[:140]
		}
		fmt.Printf("  ERR x%d  %s\n", e.v, s)
	}
}

// ---------- 连接池 dst 轮转 ----------
var dsts []string
var dstIdx int64

func nextDst() string {
	i := atomic.AddInt64(&dstIdx, 1)
	return dsts[int(i)%len(dsts)]
}

// ---------- 登录公共段 ----------
func login(c *conn, name string, deadline time.Time) error {
	if _, err := c.waitMsg(msgWelcome, deadline); err != nil {
		return err
	}
	p := append([]byte{byte(len(name))}, name...)
	if err := c.send(msgLogin, p); err != nil {
		return err
	}
	payload, err := c.waitMsg(msgLoginRes, deadline)
	if err != nil {
		return err
	}
	if len(payload) < 1 || payload[0] != 0 {
		return fmt.Errorf("login code=%v", payload)
	}
	return nil
}

// ---------- 大厅 bot：登录 + 保持 ----------
func lobbyBot(i int, deadline time.Time, done func()) {
	defer done()
	name := fmt.Sprintf("b%06d", i)
	c0, err := dial(nextDst())
	if err != nil {
		atomic.AddInt64(&c.connFail, 1)
		errAdd("dial:" + err.Error())
		return
	}
	atomic.AddInt64(&c.connOK, 1)
	if err := login(c0, name, time.Now().Add(15*time.Second)); err != nil {
		atomic.AddInt64(&c.loginFail, 1)
		errAdd("login:" + err.Error())
		c0.closeRST()
		return
	}
	atomic.AddInt64(&c.loginOK, 1)
	// 保持：大厅静默属正常；被服务器中途掐线=失败
	for {
		remain := time.Until(deadline)
		if remain <= 0 {
			break
		}
		_, _, err := c0.readFrame(time.Now().Add(remain))
		if err != nil {
			if e, ok := err.(net.Error); ok && e.Timeout() {
				break // 到点
			}
			atomic.AddInt64(&c.lobbyClosedMid, 1)
			errAdd("lobby-closed-mid:" + err.Error())
			c0.closeRST()
			return
		}
	}
	c0.closeRST()
}

// ---------- 对局 bot：全流程 ----------
type matchLag struct {
	lag  int // sent - lastAck
	rate float64
}

var lagMu sync.Mutex
var lags []matchLag

func matchBot(gidx, myIdx int, started chan uint32, wg *sync.WaitGroup, deadline time.Time) {
	defer wg.Done()
	isHost := myIdx == 0
	// 房主广播：向组内每个成员各发一份（0=建房失败，全员散伙）；通道缓冲=group，成员不读也不堵房主
	broadcast := func(v uint32) {
		if isHost {
			for k := 1; k < *group; k++ {
				started <- v
			}
		}
	}
	name := fmt.Sprintf("m%06d", gidx**group+myIdx)
	c0, err := dial(nextDst())
	if err != nil {
		atomic.AddInt64(&c.connFail, 1)
		errAdd("mdial:" + err.Error())
		broadcast(0)
		return
	}
	atomic.AddInt64(&c.connOK, 1)
	dl := time.Now().Add(20 * time.Second)
	if err := login(c0, name, dl); err != nil {
		atomic.AddInt64(&c.loginFail, 1)
		errAdd("mlogin:" + err.Error())
		c0.closeRST()
		broadcast(0)
		return
	}
	atomic.AddInt64(&c.loginOK, 1)

	var roomID uint32
	if isHost {
		// 建房（房名允许重复，身份=roomId）
		rn := fmt.Sprintf("r%06d", gidx)
		p := append([]byte{byte(len(rn))}, rn...)
		if err := c0.send(msgCreate, p); err != nil {
			atomic.AddInt64(&c.createFail, 1)
			errAdd("create-send:" + err.Error())
			c0.closeRST()
			broadcast(0)
			return
		}
		payload, err := c0.waitMsg(msgCreateRes, dl)
		if err != nil || len(payload) < 5 || payload[0] != 0 {
			atomic.AddInt64(&c.createFail, 1)
			errAdd(fmt.Sprintf("create-res:%v len=%d code=%v", err, len(payload), payload[:min(1, len(payload))]))
			c0.closeRST()
			broadcast(0)
			return
		}
		atomic.AddInt64(&c.createOK, 1)
		roomID = binary.LittleEndian.Uint32(payload[1:5])
		broadcast(roomID)
	} else {
		roomID = <-started
		if roomID == 0 {
			c0.closeRST()
			return
		}
	}
	if myIdx != 0 {
		// 等房主建房广播就绪后进房（roomID 通道同步已保证房存在）
		if err := c0.send(msgJoin, u32(roomID)); err != nil {
			atomic.AddInt64(&c.joinFail, 1)
			errAdd("join-send:" + err.Error())
			c0.closeRST()
			return
		}
		payload, err := c0.waitMsg(msgJoinRes, dl)
		if err != nil || len(payload) < 1 || payload[0] != 0 {
			atomic.AddInt64(&c.joinFail, 1)
			errAdd(fmt.Sprintf("join-res:%v code=%v", err, payload[:min(1, len(payload))]))
			c0.closeRST()
			return
		}
		atomic.AddInt64(&c.joinOK, 1)
	}
	if isHost {
		// 房主等所有成员进房的 ROOM_STATE 旁路推送攒够（n 人含己 → 再等 n-1 次成员变更），简单法：等 300ms 静默
		// 不精确等待——服务器开局按当前名册广播，若成员未到齐会少人；用 MATCH_START 名册数校验兑底
		time.Sleep(300 * time.Millisecond)
		if err := c0.send(msgStart, nil); err != nil {
			errAdd("start-send:" + err.Error())
			c0.closeRST()
			return
		}
	}

	// 等 MATCH_START（跳过 ROOM_STATE 等旁路）
	payload, err := c0.waitMsg(msgMatchStart, time.Now().Add(30*time.Second))
	if err != nil {
		atomic.AddInt64(&c.matchStartFail, 1)
		errAdd("matchstart:" + err.Error())
		c0.closeRST()
		return
	}
	if len(payload) < 4 {
		atomic.AddInt64(&c.matchStartFail, 1)
		errAdd("matchstart:short")
		c0.closeRST()
		return
	}
	nMembers := int(binary.LittleEndian.Uint32(payload[0:4]))
	if nMembers != *group {
		atomic.AddInt64(&c.matchStartFail, 1)
		errAdd(fmt.Sprintf("roster=%d want=%d", nMembers, *group))
	}
	// 名册认领自己的 pid
	off := 4
	var selfPid uint32
	for k := 0; k < nMembers && off+5 <= len(payload); k++ {
		pid := binary.LittleEndian.Uint32(payload[off : off+4])
		nl := int(payload[off+4])
		nm := string(payload[off+5 : off+5+nl])
		if nm == name {
			selfPid = pid
		}
		off += 5 + nl + 1 // pid + [nlen][name][spawn]
	}
	if selfPid == 0 {
		atomic.AddInt64(&c.matchStartFail, 1)
		errAdd("self-pid-not-in-roster")
		c0.closeRST()
		return
	}
	atomic.AddInt64(&c.matchStartOK, 1)
	// 对局窗口 = MATCH_START 时刻起算 hold 秒（与大厅保持窗口松散重叠即可；快照速率分母=hold）
	matchEnd := time.Now().Add(time.Duration(*hold * float64(time.Second)))

	// 读协程：数快照 + 追自实体 ack
	var lastAck uint32
	var snapCount int64
	var lastSnap atomic.Int64
	lastSnap.Store(time.Now().UnixNano())
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		for {
			mid, payload, err := c0.readFrame(matchEnd.Add(5 * time.Second))
			if err != nil {
				return
			}
			if mid != msgSnap {
				continue
			}
			atomic.AddInt64(&c.snapFrames, 1)
			snapCount++
			lastSnap.Store(time.Now().UnixNano())
			if len(payload) < 2 {
				continue
			}
			nn := int(binary.LittleEndian.Uint16(payload[0:2]))
			off := 2
			for k := 0; k < nn && off+29 <= len(payload); k++ {
				pid := binary.LittleEndian.Uint32(payload[off : off+4])
				if pid == selfPid {
					ack := binary.LittleEndian.Uint32(payload[off+25 : off+29])
					if ack > lastAck {
						lastAck = ack
					}
				}
				off += 29
			}
		}
	}()

	// 写循环：30Hz 诚实轨迹（圆周 R=500uu，单条位移 ≈3.5uu « 钳制界 100uu → 权威≈上报）
	interval := time.Duration(float64(time.Second) / *hz)
	tick := time.NewTicker(interval)
	defer tick.Stop()
	seq := uint32(0)
	radius := 500.0
	base := float64(gidx**group+myIdx) * 0.37
	sent := uint32(0)
	for range tick.C {
		if time.Now().After(matchEnd) {
			break
		}
		seq++
		ang := float64(seq) * (2 * math.Pi / (900)) // 900 条 ≈ 30s 一圈
		x := float32(radius * math.Cos(ang+base))
		z := float32(radius * math.Sin(ang+base))
		pl := make([]byte, 25)
		binary.LittleEndian.PutUint32(pl[0:], seq)
		binary.LittleEndian.PutUint32(pl[4:], math.Float32bits(x))
		binary.LittleEndian.PutUint32(pl[8:], math.Float32bits(0))
		binary.LittleEndian.PutUint32(pl[12:], math.Float32bits(z))
		binary.LittleEndian.PutUint32(pl[16:], math.Float32bits(float32(ang*180/math.Pi)))
		binary.LittleEndian.PutUint32(pl[20:], 0)
		pl[24] = 0
		if err := c0.send(msgInput, pl); err != nil {
			errAdd("input-send:" + err.Error())
			break
		}
		sent = seq
		atomic.AddInt64(&c.sentInputs, 1)
	}
	c0.closeRST()
	<-readerDone

	// 判卷：ack 滞后 + 快照饥饿
	lagVal := int(sent - lastAck)
	rate := float64(snapCount) / *hold
	lagMu.Lock()
	lags = append(lags, matchLag{lag: lagVal, rate: rate})
	lagMu.Unlock()
	if snapCount == 0 {
		atomic.AddInt64(&c.snapStarved, 1)
		errAdd("snap-starved(0 frames)")
	}
	if lagVal > 30 { // ack 落后 >1s = main 线程消化不动
		errAdd(fmt.Sprintf("ack-lag=%d inputs (sent=%d ack=%d)", lagVal, sent, lastAck))
	}
	if snapCount > 0 && lagVal <= 30 {
		atomic.AddInt64(&c.matchDoneOK, 1)
	} else {
		atomic.AddInt64(&c.matchDoneFail, 1)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ---------- 主流程 ----------
func main() {
	flag.Parse()
	for _, s := range strings.Split(*ipsFlag, ",") {
		s = strings.TrimSpace(s)
		if s != "" {
			dsts = append(dsts, fmt.Sprintf("%s:%d", s, *port))
		}
	}
	if *matchN%*group != 0 {
		fmt.Fprintf(os.Stderr, "matchN=%d 必须是 group=%d 的倍数\n", *matchN, *group)
		os.Exit(2)
	}
	if *matchN > *group {
		fmt.Printf("  ⚠ 服务器 ep04=单局架构（MatchService::StartMatch 会 entities_.clear()）：\n" +
			"    多房同时开局时，后房清掉前房实体（实测 ack=0/快照饿死）——多局=第 7 期的活。\n" +
			"    建议 matchN=%d（1 房全流程）；当前 %d 仅最后一房有效。\n", *group, *matchN)
	}

	fmt.Printf("[loadbot v2] dsts=%v lobby=%d match=%d(%d房×%d) ramp=%.0f/s mramp=%.0f/s hold=%.0fs hz=%.0f\n",
		dsts, *nLobby, *matchN, *matchN / *group, *group, *ramp, *mramp, *hold, *hz)
	t0 := time.Now()

	// 实时进度播报（每 5s 一行）——LoadLog04 动画复放的"实时日志"素材源（2026-10-23）
	stopTick := make(chan struct{})
	var tickWg sync.WaitGroup
	tickWg.Add(1)
	go func() {
		defer tickWg.Done()
		tk := time.NewTicker(5 * time.Second)
		defer tk.Stop()
		for {
			select {
			case <-tk.C:
				fmt.Printf("  [t+%.0fs] conn %d/%d · login %d/%d · mstart %d/%d · input %d · snap %d\n",
					time.Since(t0).Seconds(),
					atomic.LoadInt64(&c.connOK), atomic.LoadInt64(&c.connFail),
					atomic.LoadInt64(&c.loginOK), atomic.LoadInt64(&c.loginFail),
					atomic.LoadInt64(&c.matchStartOK), atomic.LoadInt64(&c.matchStartFail),
					atomic.LoadInt64(&c.sentInputs), atomic.LoadInt64(&c.snapFrames))
			case <-stopTick:
				return
			}
		}
	}()

	// —— 阶段 1：对局连接 + 开局（房间列表推送只打到少数未进房连接，避开 O(N²) 风暴）——
	var wg sync.WaitGroup
	if *matchN > 0 {
		perGroup := make([]chan uint32, *matchN / *group)
		gDone := make(chan struct{})
		var gWg sync.WaitGroup
		for g := 0; g < *matchN / *group; g++ {
			perGroup[g] = make(chan uint32, *group)
			for m := 0; m < *group; m++ {
				gWg.Add(1)
				gg, mm := g, m
				wg.Add(1)
				go func() {
					defer wg.Done()
					matchBot(gg, mm, perGroup[gg], &gWg, time.Now().Add(time.Duration(*hold+60)*time.Second))
				}()
				// mramp 限速
				time.Sleep(time.Duration(float64(time.Second) / *mramp))
			}
		}
		go func() { gWg.Wait(); close(gDone) }()
		select {
		case <-gDone:
		case <-time.After(time.Duration(*hold+90) * time.Second):
			fmt.Println("  ⚠ 对局开局超时（部分组未完成）")
		}
		fmt.Printf("  [phase1] 对局开局完成 @%.1fs  matchStart ok/fail=%d/%d\n",
			time.Since(t0).Seconds(), atomic.LoadInt64(&c.matchStartOK), atomic.LoadInt64(&c.matchStartFail))
	}

	// —— 阶段 2：大厅铺设 → 共同步保持 ——
	var lWg sync.WaitGroup
	lobbyStart := time.Now()
	for i := 0; i < *nLobby; i++ {
		lWg.Add(1)
		deadline := lobbyStart.Add(time.Duration(*hold) * time.Second).Add(time.Duration(float64(time.Second) / *ramp * float64(*nLobby)))
		go lobbyBot(i, deadline, lWg.Done)
		time.Sleep(time.Duration(float64(time.Second) / *ramp))
	}
	lobbyDeadline := time.Now().Add(time.Duration(*hold) * time.Second)
	_ = lobbyDeadline // 保持窗口锚点：lobbyBot 内部 deadline 已各自计算；此处等全部退出
	lWg.Wait()
	wg.Wait()

	close(stopTick)
	tickWg.Wait()

	dt := time.Since(t0)
	fmt.Printf("[loadbot v2] 总耗时 %.1fs\n", dt.Seconds())
	fmt.Printf("  conn   ok/fail        %d/%d\n", atomic.LoadInt64(&c.connOK), atomic.LoadInt64(&c.connFail))
	fmt.Printf("  login  ok/fail        %d/%d\n", atomic.LoadInt64(&c.loginOK), atomic.LoadInt64(&c.loginFail))
	if *matchN > 0 {
		fmt.Printf("  room   create/join    %d/%d (fail %d/%d)\n",
			atomic.LoadInt64(&c.createOK), atomic.LoadInt64(&c.joinOK),
			atomic.LoadInt64(&c.createFail), atomic.LoadInt64(&c.joinFail))
		fmt.Printf("  match  start ok/fail  %d/%d\n", atomic.LoadInt64(&c.matchStartOK), atomic.LoadInt64(&c.matchStartFail))
		fmt.Printf("  match  done ok/fail   %d/%d\n", atomic.LoadInt64(&c.matchDoneOK), atomic.LoadInt64(&c.matchDoneFail))
		fmt.Printf("  input  sent           %d (%.0f/s 全场)\n", atomic.LoadInt64(&c.sentInputs), float64(atomic.LoadInt64(&c.sentInputs)) / *hold)
		fmt.Printf("  snap   frames         %d\n", atomic.LoadInt64(&c.snapFrames))
		fmt.Printf("  snap   starved        %d\n", atomic.LoadInt64(&c.snapStarved))
		if len(lags) > 0 {
			var rl []float64
			var ll []int
			for _, l := range lags {
				rl = append(rl, l.rate)
				ll = append(ll, l.lag)
			}
			sort.Float64s(rl)
			sort.Ints(ll)
			pctF := func(s []float64, q float64) float64 { return s[int(float64(len(s)-1)*q)] }
			pctI := func(s []int, q float64) int { return s[int(float64(len(s)-1)*q)] }
			fmt.Printf("  snap   rate/s p50/p99/min = %.1f / %.1f / %.1f（应≈%.0f）\n",
				pctF(rl, 0.5), pctF(rl, 0.99), rl[0], *hz)
			fmt.Printf("  ack    lag p50/p99/max    = %d / %d / %d（输入条数）\n",
				pctI(ll, 0.5), pctI(ll, 0.99), ll[len(ll)-1])
		}
	}
	fmt.Printf("  lobby closed-mid      %d\n", atomic.LoadInt64(&c.lobbyClosedMid))
	errDump()

	bad := atomic.LoadInt64(&c.connFail) + atomic.LoadInt64(&c.loginFail) +
		atomic.LoadInt64(&c.lobbyClosedMid) + atomic.LoadInt64(&c.matchDoneFail) +
		atomic.LoadInt64(&c.createFail) + atomic.LoadInt64(&c.joinFail)
	if bad > 0 {
		os.Exit(1)
	}
	fmt.Println("  全绿 ✓")
}
