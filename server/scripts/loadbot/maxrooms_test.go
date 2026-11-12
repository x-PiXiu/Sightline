// maxrooms_test.go —— ep09 步骤 8：多房梯度压测——**测出最大稳定房间数**（每房 4 人满员）
//
// 形态（按需求）：服务端正常启动（用户手动起），Go 模拟真人开房进房对局，梯度加压。
//   跑法：SIGHTLINE_E2E_EXTERNAL=1 ~/go/bin/go test -v -timeout 900s -run TestMaxRooms .
//   目标：SIGHTLINE_STRESS_ADDR（默认 127.0.0.1:18888——用户正常启动的实例）
//
// 梯度：1→2→4→8→…→512 房 × 每房 4 人（满员·kMaxRoomMembers 上限）
// 流程（真人全链）：登录 → 建房 → 3 成员进房 → 房主开局 → 4 人等 15 号名册
//                 → 30Hz 上报维持 5s（首报出生点·rebase 门 → 绕出生点圆周）
// 判据（每房全部满足·KPI §6.4 口径）：
//   ① 快照 ≥29 帧/s（30Hz 保持——帧数按实测 elapsed 折算）
//   ② 自实体 ack 滞后 ≤30（1s 内——loadbot 同款判据）
//   ③ 无断连/无读写错误
// 结论：首个失败档的上一档 = **最大稳定房间数**（t.Logf 输出——环境相关不设断言下界）。
//
// 服务器模式注记：默认 Serial（主线程顺序 TickAll——N 房线性吃帧预算，天花板=真实串行极限）；
//   SIGHTLINE_WORKERS=1 启动=PinnedWorker 分桶（房间绑定 worker，天花板应显著更高）——
//   两种模式各跑一次即得 ep09 分桶收益的直接对照（周报 KPI 素材）。
package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"net"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

// maintainRes：单连接维持期统计（判据数据源）
type maintainRes struct {
	roomIdx, connIdx int
	selfPid          uint32
	frames           int     // 快照帧数
	sent, ack        uint32  // 上报序 / 自实体最新 ackedSeq
	elapsed          float64 // 秒
	err              string
}

// maintainConn：单连接 30Hz 上报 + 收 17 号快照（5s 窗口）——不能用 t.Fatal（子 goroutine），错误收集
func maintainConn(roomIdx, connIdx int, c *conn, selfPid uint32, sp spawnPt, dur time.Duration, out chan<- maintainRes) {
	res := maintainRes{roomIdx: roomIdx, connIdx: connIdx, selfPid: selfPid}
	defer func() { out <- res }()
	seq := uint32(0)
	base := float64(roomIdx*4+connIdx) * 0.37

	// —— 阶段 1：激活（seq1 出生点首报 → 轮询等快照含自己）——
	// ⚠ 真实边界（32 房档实锤）：服务器帧变慢时 drain 间隔>33ms 发包间隔，newest-wins 会把
	//   seq1（出生点首报）覆盖成远点 Move → rebase 门全拒 → 永不激活（sent=145 ack=0）。
	//   真实 UE 客户端同理——首帧上报后须先被服务器消费才开始持续流。故：先确认激活再开压，
	//   让判据测"30Hz 保持"而非"激活竞态"。
	if err := sendInput(c, 1, float32(sp.X), float32(sp.Y), float32(sp.Z), 0); err != nil {
		res.err = "send-first:" + err.Error()
		return
	}
	res.sent = 1
	activated := false
	actDeadline := time.Now().Add(2 * time.Second)
	for !activated && time.Now().Before(actDeadline) {
		mid, pl, err := c.readFrame(time.Now().Add(200 * time.Millisecond))
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			res.err = "read(act):" + err.Error()
			return
		}
		if mid != msgSnap || len(pl) < 2 {
			continue
		}
		res.frames++
		nn := int(binary.LittleEndian.Uint16(pl[0:2]))
		off := 2
		for k := 0; k < nn && off+29 <= len(pl); k++ {
			if binary.LittleEndian.Uint32(pl[off:off+4]) == res.selfPid {
				activated = true
				if ack := binary.LittleEndian.Uint32(pl[off+25 : off+29]); ack > res.ack {
					res.ack = ack
				}
			}
			off += 29
		}
	}
	if !activated {
		res.err = "activation-timeout(2s)：首报未被消费（rebase 门/路由）"
		return
	}

	// —— 阶段 2：30Hz 真人节奏维持 + 收 17 号快照 ——
	maintStart := time.Now() // elapsed 只算维持段（激活期帧稀会拉低 fps 误判）
	deadline := maintStart.Add(dur)
	tick := time.NewTicker(time.Second / 30)
	defer tick.Stop()
	for time.Now().Before(deadline) {
		select {
		case <-tick.C:
			seq++
			ang := float64(seq) * (2 * math.Pi / 900)
			x := float32(sp.X + 500*math.Cos(ang+base))
			y := float32(sp.Y + 500*math.Sin(ang+base))
			z := float32(sp.Z)
			yaw := float32(ang * 180 / math.Pi)
			if err := sendInput(c, seq, x, y, z, yaw); err != nil {
				res.err = "send:" + err.Error()
				return
			}
			res.sent = seq
		default:
		}
		// 非阻塞收帧（短 deadline 切片）
		mid, pl, err := c.readFrame(time.Now().Add(time.Second / 30))
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			res.err = "read:" + err.Error() // 断连
			return
		}
		if mid != msgSnap || len(pl) < 2 {
			continue
		}
		res.frames++
		nn := int(binary.LittleEndian.Uint16(pl[0:2]))
		off := 2
		for k := 0; k < nn && off+29 <= len(pl); k++ {
			if binary.LittleEndian.Uint32(pl[off:off+4]) == res.selfPid {
				if ack := binary.LittleEndian.Uint32(pl[off+25 : off+29]); ack > res.ack {
					res.ack = ack
				}
			}
			off += 29
		}
	}
	res.elapsed = time.Since(maintStart).Seconds()
}

// gradeOnce：单档 n 房×4 人全流程——返回 (全房达标?, 首个失败原因)
// 结构：串行建房+开局（测试 goroutine·可用 Fatal——40ms/房 量级）→ 并行维持 5s（4n goroutine）
//       → 房级判据聚合（任一连接失败=该档失败）
func gradeOnce(t *testing.T, n int) (bool, string) {
	t.Helper()
	spawns := loadSpawnPoints(t)
	nano := time.Now().UnixNano() % 1000000 // 名字防撞（同服务器连跑）

	type room struct {
		conns [4]*conn
		pids  [4]uint32
		spx   [4]int // spawnIndex（首报出生点用）
	}
	rooms := make([]room, n)
	buildStart := time.Now() // 归因探针信号：建房完成时刻（探针轮询"就绪"行→采样维持期）
	defer func() { // 全量 RST 清理（loadbot 血统：不留 TIME_WAIT）
		for i := range rooms {
			for _, c := range rooms[i].conns {
				if c != nil {
					c.closeRST()
				}
			}
		}
	}()

	// —— 串行：每房 建→3 进→房主开→全员等 15 号 ——
	for i := 0; i < n; i++ {
		var rid uint32
		names := [4]string{}
		for j := 0; j < 4; j++ {
			names[j] = fmt.Sprintf("%x%x%x%x", n, i, nano&0xFFFF, j) // ≤16B（ValidLoginName 上限——128 档曾17B 被拒 code=2）
			c, err := dial(targetAddr)
			if err != nil {
				return false, fmt.Sprintf("房%d 连接%d dial:%v", i, j, err)
			}
			rooms[i].conns[j] = c
			if err := login(c, names[j], time.Now().Add(10*time.Second)); err != nil {
				return false, fmt.Sprintf("房%d 连接%d login:%v", i, j, err)
			}
			if j == 0 { // 房主建房（建房者自动入房）
				rn := fmt.Sprintf("r%d_%d_%d", n, i, nano)
				p := append([]byte{byte(len(rn))}, rn...)
				if err := c.send(msgCreate, p); err != nil {
					return false, fmt.Sprintf("房%d create:%v", i, err)
				}
				pl, err := c.waitMsg(msgCreateRes, time.Now().Add(5*time.Second))
				if err != nil || len(pl) < 5 || pl[0] != 0 {
					return false, fmt.Sprintf("房%d create-res:%v code=%v", i, err, pl[:min(1, len(pl))])
				}
				rid = binary.LittleEndian.Uint32(pl[1:5])
			} else { // 成员进房
				if err := c.send(msgJoin, u32(rid)); err != nil {
					return false, fmt.Sprintf("房%d join-send:%v", i, err)
				}
				pl, err := c.waitMsg(msgJoinRes, time.Now().Add(5*time.Second))
				if err != nil || len(pl) < 1 || pl[0] != 0 {
					return false, fmt.Sprintf("房%d join-res:%v code=%v", i, err, pl[:min(1, len(pl))])
				}
			}
		}
		// 房主开局（成员已全部就位——串行保证，无需 300ms 盲等）
		if err := rooms[i].conns[0].send(msgStart, nil); err != nil {
			return false, fmt.Sprintf("房%d start:%v", i, err)
		}
		// 全员等 15 号名册（房主发过 start → 只等；成员靠广播）
		for j := 0; j < 4; j++ {
			pid, _, spx := waitRosterOnly(t, rooms[i].conns[j], 4, names[j])
			rooms[i].pids[j] = pid
			rooms[i].spx[j] = spx
		}
	}

	// —— 并行维持 5s（4n goroutine·30Hz 上报+收快照）——
	t.Logf("房全部就绪（建房 %.1fs）→ 进入维持", time.Since(buildStart).Seconds()) // 归因探针采样信号
	const maintain = 5 * time.Second
	resCh := make(chan maintainRes, 4*n)
	var wg sync.WaitGroup
	for i := range rooms {
		for j := 0; j < 4; j++ {
			sp := spawns[0] // 越界保险（spx 异常回退 0 号出生点）
			if rooms[i].spx[j] >= 0 && rooms[i].spx[j] < len(spawns) {
				sp = spawns[rooms[i].spx[j]]
			}
			wg.Add(1)
			go func(i, j int, sp spawnPt) {
				defer wg.Done()
				maintainConn(i, j, rooms[i].conns[j], rooms[i].pids[j],
					sp, maintain, resCh)
			}(i, j, sp)
		}
	}
	wg.Wait()
	close(resCh)

	// —— 房级判据聚合 ——
	for res := range resCh {
		if res.err != "" {
			return false, fmt.Sprintf("房%d 连%d 断连/错误: %s", res.roomIdx, res.connIdx, res.err)
		}
		if res.elapsed < 4.5 {
			return false, fmt.Sprintf("房%d 连%d 维持窗过短 %.2fs", res.roomIdx, res.connIdx, res.elapsed)
		}
		fps := float64(res.frames) / res.elapsed
		if fps < 29 {
			return false, fmt.Sprintf("房%d 连%d 快照 %.1f 帧/s <29（30Hz 保持失败）", res.roomIdx, res.connIdx, fps)
		}
		if res.sent > res.ack && res.sent-res.ack > 30 {
			return false, fmt.Sprintf("房%d 连%d ack 滞后 %d>30（sent=%d ack=%d）",
				res.roomIdx, res.connIdx, res.sent-res.ack, res.sent, res.ack)
		}
	}
	return true, ""
}

// TestMaxRoomsStress：梯度压测主入口
func TestMaxRoomsStress(t *testing.T) {
	stressAddr := envOr("SIGHTLINE_STRESS_ADDR", "127.0.0.1:18888")
	// 探测外部服务器（正常启动的实例）
	probe, err := net.DialTimeout("tcp", stressAddr, 2*time.Second)
	if err != nil {
		t.Skipf("外部服务器未启动（%s）——先正常启动：\n"+
			"  ./build.sh server && SIGHTLINE_PORT=18888 ./sightline_server\n"+
			"  （分桶模式对比：SIGHTLINE_WORKERS=1 起再跑一次）\n"+
			"  然后：SIGHTLINE_E2E_EXTERNAL=1 go test -run TestMaxRooms -v .", stressAddr)
	}
	_ = probe.Close()
	targetAddr = stressAddr // 全部辅助切到外部实例

	grades := []int{1, 2, 4, 8, 16, 32, 64, 128, 256, 512}
	if g := os.Getenv("SIGHTLINE_STRESS_GRADES"); g != "" { // 单档/子集（perf 归因窗口可控）
		grades = nil
		for _, s := range strings.Split(g, ",") {
			var v int
			if _, err := fmt.Sscanf(s, "%d", &v); err == nil && v > 0 {
				grades = append(grades, v)
			}
		}
		if len(grades) == 0 {
			t.Fatalf("SIGHTLINE_STRESS_GRADES 解析失败: %q", g)
		}
	}
	maxOk := 0
	for _, n := range grades {
		ok, reason := gradeOnce(t, n)
		if !ok {
			t.Logf("✗ %3d 房×4人 失败：%s", n, reason)
			t.Logf("★ 最大稳定房间数 = %d（每房4人·快照≥29fps·ack≤30·无断连）", maxOk)
			if maxOk == 0 {
				t.Fatalf("1 房都不达标——链路级故障（先跑功能用例定位）")
			}
			return
		}
		maxOk = n
		t.Logf("✓ %3d 房×4人 全部达标（30Hz 保持）", n)
	}
	t.Logf("★ 最大稳定房间数 ≥ %d（梯度上限——环境有余量）", maxOk)
}
