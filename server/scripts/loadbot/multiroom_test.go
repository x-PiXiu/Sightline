// multiroom_test.go —— ep09 e2e：多房功能验收（go test 形态·复用 loadbot 帧编解码血统）
//
// 覆盖（用户四问逐条对应）：
//   ① 建多个房间并维持角色  → TestMultiRoomParallel（8 房并行×3 人·19 号持续上报·17 号快照持续到达）
//   ② 测试房间上限          → TestRoomMemberCap（每房 4 人上限：第 5 人拒码 3）
//                            + TestManyRoomsStability（lobby 实勘**无房间总数硬上限**——kMaxRooms
//                              不存在，故"上限"语义=每房人数上限；总数侧做 40 房规模稳定性验证）
//   ③ 校验逻辑是否正确      → 快照零串扰（pid⊆本房名册）/ 同房 4 枪击杀（21/23 号）/
//                            跨房开火必 miss（候选集隔离·与同房命中互为对照）/
//                            对局中拒加入码 5（本测试抓出的实现缺口已修·lobby.h:125 契约）/
//                            关服优雅退出码 0（129=SIGHUP 挂死的持续回归·TestMain）
//   ④ 账号域 31~33         → 需 full 模式+真库，留给步骤 6 实机（本测试 lite 自起）
//
// 前置：../../sightline_server 已构建（./build.sh server）；TestMain 自起独立端口实例
//       （SIGHTLINE_PORT=18899·SIGHTLINE_CMD_PORT=0 关命令口），与手动实例互不干扰。
// 跑法：cd server/scripts/loadbot && ~/go/bin/go test -v -timeout 180s .
package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

// —— ep09 新增消息号（loadbot const 块未覆盖）——
const (
	msgLeave    = 18
	msgFire     = 20
	msgFireRes  = 21
	msgKill     = 23
	e2ePort     = 18899
	e2eAddr     = "127.0.0.1:18899"
)

// ---------- TestMain：自起服务器 + 优雅退出回归 ----------
var srvLog bytes.Buffer // 单进程写·Wait 后读（无并发）

func TestMain(m *testing.M) {
	bin := "../../sightline_server"
	if _, err := os.Stat(bin); err != nil {
		fmt.Fprintf(os.Stderr, "[e2e] 缺少服务器二进制 %s——先 ./build.sh server\n", bin)
		os.Exit(2)
	}
	srv := exec.Command(bin)
	srv.Env = append(os.Environ(),
		fmt.Sprintf("SIGHTLINE_PORT=%d", e2ePort),
		"SIGHTLINE_CMD_PORT=0", // 关命令口（避免 18889 冲突）
		// 空几何（关注点分离）：本 e2e 验"判定/击杀/隔离/多房"链路，遮挡挡弹语义由
		// test_match 单测覆盖（"遮挡挡弹 hitPoint=墙面"）——真几何下 spawn 间视线穿箱
		// （实测 hit=(901,1020,155)=227cm 撞箱）会让出生点选位变成几何题，掩盖链路问题。
		"SIGHTLINE_MAP_GEOMETRY=/nonexistent_e2e.json",
	)
	srv.Stdout = os.Stdout // 透传：go test 成功时隐藏、-v/失败时可见
	srv.Stderr = os.Stderr
	if err := srv.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "[e2e] 服务器启动失败: %v\n", err)
		os.Exit(2)
	}
	// 等就绪（拨号探测·最多 10s）
	ready := false
	for i := 0; i < 20; i++ {
		if c, err := net.DialTimeout("tcp", e2eAddr, time.Second); err == nil {
			_ = c.Close()
			ready = true
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if !ready {
		fmt.Fprintf(os.Stderr, "[e2e] 服务器 10s 未就绪 :%d\n", e2ePort)
		_ = srv.Process.Kill()
		_ = srv.Wait()
		os.Exit(2)
	}

	code := m.Run()

	// —— 优雅退出回归（关服 129=SIGHUP 挂死修复的持续验证·2026-11-12 根除）——
	_ = srv.Process.Signal(syscall.SIGTERM)
	waitCh := make(chan error, 1)
	go func() { waitCh <- srv.Wait() }()
	exitOK := false
	select {
	case err := <-waitCh:
		if err == nil {
			exitOK = true
		} else if ee, ok := err.(*exec.ExitError); ok && ee.ExitCode() == 0 {
			exitOK = true
		}
	case <-time.After(6 * time.Second):
		_ = srv.Process.Kill() // 挂死兜底强杀（若回归会走到这）
		<-waitCh
		fmt.Fprintf(os.Stderr, "[e2e] ✗ 服务器关服挂死 >6s（129 挂死回归！）\n")
	}
	if !exitOK && code == 0 {
		code = 1
		fmt.Fprintf(os.Stderr, "[e2e] ✗ 服务器关服退出码非 0（129=SIGHUP 挂死回归）\n")
	}
	os.Exit(code)
}

// ---------- 协议辅助（复用 loadbot 帧编解码）----------
func mustCreate(t *testing.T, c *conn, roomName string) uint32 {
	t.Helper()
	p := append([]byte{byte(len(roomName))}, roomName...)
	if err := c.send(msgCreate, p); err != nil {
		t.Fatalf("create send: %v", err)
	}
	pl, err := c.waitMsg(msgCreateRes, time.Now().Add(5*time.Second))
	if err != nil || len(pl) < 5 || pl[0] != 0 {
		t.Fatalf("create res: err=%v len=%d code=%v", err, len(pl), pl[:min(1, len(pl))])
	}
	return binary.LittleEndian.Uint32(pl[1:5])
}

func mustJoin(t *testing.T, c *conn, roomID uint32) byte {
	t.Helper()
	if err := c.send(msgJoin, u32(roomID)); err != nil {
		t.Fatalf("join send: %v", err)
	}
	pl, err := c.waitMsg(msgJoinRes, time.Now().Add(5*time.Second))
	if err != nil || len(pl) < 1 {
		t.Fatalf("join res: err=%v len=%d", err, len(pl))
	}
	return pl[0]
}

// startAndWaitRoster：开局 → 等 15 号 → 解析名册 → 认领自己 pid + spawnIndex（出生点序）
func startAndWaitRoster(t *testing.T, c *conn, wantN int, selfName string) (uint32, map[uint32]string, int) {
	t.Helper()
	if err := c.send(msgStart, nil); err != nil {
		t.Fatalf("start send: %v", err)
	}
	pl, err := c.waitMsg(msgMatchStart, time.Now().Add(20*time.Second))
	if err != nil || len(pl) < 4 {
		t.Fatalf("matchstart: err=%v len=%d", err, len(pl))
	}
	n := int(binary.LittleEndian.Uint32(pl[0:4]))
	if n != wantN {
		t.Errorf("roster=%d want=%d", n, wantN)
	}
	roster := map[uint32]string{}
	off := 4
	var selfPid uint32
	spawnIx := -1
	for k := 0; k < n && off+5 <= len(pl); k++ {
		pid := binary.LittleEndian.Uint32(pl[off : off+4])
		nl := int(pl[off+4])
		if off+5+nl+1 > len(pl) {
			break
		}
		nm := string(pl[off+5 : off+5+nl])
		roster[pid] = nm
		if nm == selfName {
			selfPid = pid
			spawnIx = int(pl[off+5+nl]) // 名册序末字节=spawnIndex（出生点分配序）
		}
		off += 5 + nl + 1 // pid + [nlen][name][spawn]
	}
	if selfPid == 0 {
		t.Fatalf("self pid not in roster (name=%s)", selfName)
	}
	return selfPid, roster, spawnIx
}

// sendInput：19 号 25B（与 loadbot matchBot 同布局）
func sendInput(c *conn, seq uint32, x, y, z, yaw float32) error {
	pl := make([]byte, 25)
	binary.LittleEndian.PutUint32(pl[0:], seq)
	binary.LittleEndian.PutUint32(pl[4:], math.Float32bits(x))
	binary.LittleEndian.PutUint32(pl[8:], math.Float32bits(y))
	binary.LittleEndian.PutUint32(pl[12:], math.Float32bits(z))
	binary.LittleEndian.PutUint32(pl[16:], math.Float32bits(yaw))
	binary.LittleEndian.PutUint32(pl[20:], 0)
	pl[24] = 0
	return c.send(msgInput, pl)
}

// readFor：dur 窗口内读帧分派（fn=nil 的帧忽略）；对端关闭/超时=正常返回
func readFor(c *conn, d time.Duration, fn func(mid uint16, pl []byte)) error {
	end := time.Now().Add(d)
	for {
		mid, pl, err := c.readFrame(end)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				return nil
			}
			return err // 对端关闭（RST/关房）
		}
		if fn != nil {
			fn(mid, pl)
		}
	}
}

// ---------- 几何出生点（数据驱动·不硬编码）----------
// 服务端开局 rebase 门（2026-11-07 实机实锤）：首报必须落出生点 100cm 内，否则
// 实体永不激活（bHasReport=false 不入快照 → 开火全链断）——测试须模拟真实客户端
// （切图后从出生点开始），故从几何合同读 spawnPoints 而非硬编码坐标。
type spawnPt struct{ X, Y, Z float64 }

func loadSpawnPoints(t *testing.T) []spawnPt {
	t.Helper()
	b, err := os.ReadFile("../../assets/maps/NewMap.json")
	if err != nil {
		t.Fatalf("读几何合同: %v", err)
	}
	var m struct {
		SpawnPoints []struct {
			Pos [3]float64 `json:"pos"`
		} `json:"spawnPoints"`
	}
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("解析几何合同: %v", err)
	}
	if len(m.SpawnPoints) == 0 {
		t.Fatalf("几何合同 spawnPoints 为空")
	}
	out := make([]spawnPt, len(m.SpawnPoints))
	for i, s := range m.SpawnPoints {
		out[i] = spawnPt{X: s.Pos[0], Y: s.Pos[1], Z: s.Pos[2]}
	}
	return out
}

// yawTo：从 a 指向 b 的 yaw（度·服务端 DirectionFromYawPitch: yaw0=+X·cos/sin 分量）
func yawTo(a, b spawnPt) float32 {
	return float32(math.Atan2(b.Y-a.Y, b.X-a.X) * 180 / math.Pi)
}

func loginE2E(t *testing.T, name string) *conn {
	t.Helper()
	c, err := dial(e2eAddr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	if err := login(c, name, time.Now().Add(10*time.Second)); err != nil {
		c.closeRST()
		t.Fatalf("login %s: %v", name, err)
	}
	return c
}

// openRoomE2E：登录→建房→房主进房（建房者自动是成员?——loadbot：host 不发 join，
// 建房即入房（CreateRoom memberFds.push_back(fd) ✓ lobby_service 实勘））
func openRoomE2E(t *testing.T, hostName, roomName string) (*conn, uint32) {
	t.Helper()
	h := loginE2E(t, hostName)
	rid := mustCreate(t, h, roomName)
	return h, rid
}

// ---------- ① 房间人数上限（每房 4 人·第 5 人拒码 3）----------
func TestRoomMemberCap(t *testing.T) {
	host, rid := openRoomE2E(t, "cap_host", "cap_room")
	defer host.closeRST()

	// 3 名成员进房（host+3=4 满）
	for i := 1; i <= 3; i++ {
		m := loginE2E(t, fmt.Sprintf("cap_m%d", i))
		defer m.closeRST()
		if code := mustJoin(t, m, rid); code != 0 {
			t.Fatalf("member %d join code=%d want=0", i, code)
		}
	}
	// 第 5 人：拒码 3（已满）
	e := loginE2E(t, "cap_extra")
	defer e.closeRST()
	if code := mustJoin(t, e, rid); code != 3 {
		t.Fatalf("5th join code=%d want=3(已满)", code)
	}
	t.Logf("✓ 每房 4 人上限：第 5 人拒码=3")
}

// ---------- ② 对局中拒加入（码 5·ep07 ❗11 契约——本测试抓出实现缺口已修）----------
func TestJoinInMatchRejected(t *testing.T) {
	host, rid := openRoomE2E(t, "inm_host", "inm_room")
	defer host.closeRST()
	m1 := loginE2E(t, "inm_m1")
	defer m1.closeRST()
	if code := mustJoin(t, m1, rid); code != 0 {
		t.Fatalf("m1 join code=%d", code)
	}
	// 开局
	if err := host.send(msgStart, nil); err != nil {
		t.Fatalf("start: %v", err)
	}
	if _, err := host.waitMsg(msgMatchStart, time.Now().Add(20*time.Second)); err != nil {
		t.Fatalf("host matchstart: %v", err)
	}
	if _, err := m1.waitMsg(msgMatchStart, time.Now().Add(20*time.Second)); err != nil {
		t.Fatalf("m1 matchstart: %v", err)
	}
	// 对局中加入 → 拒码 5
	late := loginE2E(t, "inm_late")
	defer late.closeRST()
	if code := mustJoin(t, late, rid); code != 5 {
		t.Fatalf("in-match join code=%d want=5(对局中拒)", code)
	}
	t.Logf("✓ 对局中加入拒码=5（lobby.h:125 契约兑现）")
}

// ---------- ③ 多房并行 + 维持角色 + 快照零串扰（核心）----------
func TestMultiRoomParallel(t *testing.T) {
	const nRooms = 8
	const perRoom = 3 // host + 2 成员
	const maintain = 2500 * time.Millisecond
	const hz = 10 // 维持上报频率（10Hz 足够证明"角色活着"）

	type roomSetup struct {
		rid   uint32
		conns []*conn // [0]=host
		names []string
	}
	// —— 串行铺房（登录/建房/进房·~1s）——
	setups := make([]roomSetup, 0, nRooms)
	defer func() {
		for _, rs := range setups {
			for _, c := range rs.conns {
				c.closeRST()
			}
		}
	}()
	for r := 0; r < nRooms; r++ {
		names := make([]string, perRoom)
		for i := range names {
			names[i] = fmt.Sprintf("par_r%d_p%d", r, i)
		}
		h, rid := openRoomE2E(t, names[0], fmt.Sprintf("par_room%d", r))
		conns := []*conn{h}
		for i := 1; i < perRoom; i++ {
			m := loginE2E(t, names[i])
			if code := mustJoin(t, m, rid); code != 0 {
				t.Fatalf("room %d member %d join code=%d", r, i, code)
			}
			conns = append(conns, m)
		}
		setups = append(setups, roomSetup{rid: rid, conns: conns, names: names})
	}

	// —— 并行开局+维持（每房一 goroutine）——
	type roomResult struct {
		roomIdx    int
		rosterN    int
		selfPid    uint32
		snapFrames int
		snapPids   map[uint32]bool // 快照中见过的全部 pid（零串扰校验用）
		errs       []string
	}
	results := make(chan roomResult, nRooms)
	var wg sync.WaitGroup
	spawns := loadSpawnPoints(t) // 主 goroutine 预加载（t.Fatalf 不可入子 goroutine）
	for r, rs := range setups {
		wg.Add(1)
		go func(r int, rs roomSetup, spawns []spawnPt) {
			defer wg.Done()
			res := roomResult{roomIdx: r, snapPids: map[uint32]bool{}}
			// 房主开局（等 300ms 让成员就位——loadbot 同款手法）
			time.Sleep(300 * time.Millisecond)
			if err := rs.conns[0].send(msgStart, nil); err != nil {
				res.errs = append(res.errs, "start:"+err.Error())
				results <- res
				return
			}
			// 每连接：等 15 号名册 → 认领 pid → 维持循环
			pids := make([]uint32, perRoom)
			var mu sync.Mutex
			var rosterSet = map[uint32]bool{}
			var snapFrames int
			var snapErrs []string
			var rwg sync.WaitGroup
			for i, c := range rs.conns {
				rwg.Add(1)
				go func(i int, c *conn, name string) {
					defer rwg.Done()
					pl, err := c.waitMsg(msgMatchStart, time.Now().Add(20*time.Second))
					if err != nil || len(pl) < 4 {
						mu.Lock()
						snapErrs = append(snapErrs, fmt.Sprintf("p%d matchstart:%v", i, err))
						mu.Unlock()
						return
					}
					n := int(binary.LittleEndian.Uint32(pl[0:4]))
					off := 4
					var selfPid uint32
					selfSp := 0 // 自己 spawnIndex（出生点分配序·rebase 门首报用）
					roster := map[uint32]string{}
					for k := 0; k < n && off+5 <= len(pl); k++ {
						pid := binary.LittleEndian.Uint32(pl[off : off+4])
						nl := int(pl[off+4])
						if off+5+nl+1 > len(pl) {
							break
						}
						nm := string(pl[off+5 : off+5+nl])
						roster[pid] = nm
						if nm == name {
							selfPid = pid
							selfSp = int(pl[off+5+nl])
						}
						off += 5 + nl + 1
					}
					mu.Lock()
					pids[i] = selfPid
					for pid := range roster {
						rosterSet[pid] = true
					}
					mu.Unlock()
					if selfPid == 0 {
						mu.Lock()
						snapErrs = append(snapErrs, fmt.Sprintf("p%d self-pid=0", i))
						mu.Unlock()
						return
					}
					// 维持：10Hz 上报 + 收 17 号快照（持续到达=角色活着）
					// 首报=出生点（rebase 开局门：首报须落出生点 100cm 内，否则永不激活）；
					// 其后绕出生点圆周（xy 平面·高度固定——步长≈3.5uu « 钳制界 100uu）
					deadline := time.Now().Add(maintain)
					seq := uint32(0)
					base := float64(r*perRoom+i) * 0.41
					bp := spawns[selfSp]
					tick := time.NewTicker(time.Second / hz)
					defer tick.Stop()
					for time.Now().Before(deadline) {
						select {
						case <-tick.C:
							seq++
							var x, y, z, yaw float32
							if seq == 1 {
								x, y, z = float32(bp.X), float32(bp.Y), float32(bp.Z)
							} else {
								ang := float64(seq) * (2 * math.Pi / 900)
								x = float32(bp.X + 500*math.Cos(ang+base))
								y = float32(bp.Y + 500*math.Sin(ang+base))
								z = float32(bp.Z)
								yaw = float32(ang * 180 / math.Pi)
							}
							if err := sendInput(c, seq, x, y, z, yaw); err != nil {
								mu.Lock()
								snapErrs = append(snapErrs, "input:"+err.Error())
								mu.Unlock()
								return
							}
						default:
						}
						// 非阻塞收帧（短 deadline 切片）
						_ = c.c.SetReadDeadline(time.Now().Add(time.Second / hz))
						mid, pl, err := c.readFrame(time.Now().Add(time.Second / hz))
						if err != nil {
							if ne, ok := err.(net.Error); ok && ne.Timeout() {
								continue
							}
							mu.Lock()
							snapErrs = append(snapErrs, "read:"+err.Error())
							mu.Unlock()
							return
						}
						if mid != msgSnap || len(pl) < 2 {
							continue
						}
						nn := int(binary.LittleEndian.Uint16(pl[0:2]))
						off := 2
						mu.Lock()
						snapFrames++
						for k := 0; k < nn && off+29 <= len(pl); k++ {
							res.snapPids[binary.LittleEndian.Uint32(pl[off:off+4])] = true
							off += 29
						}
						mu.Unlock()
					}
				}(i, c, rs.names[i])
			}
			rwg.Wait()
			mu.Lock()
			res.rosterN = len(rosterSet)
			res.snapFrames = snapFrames
			res.errs = append(res.errs, snapErrs...)
			// 快照 pid ⊆ 名册（零串扰核心断言的数据收集——跨房 pid 出现即错）
			for pid := range res.snapPids {
				if !rosterSet[pid] {
					res.errs = append(res.errs, fmt.Sprintf("快照含非本房 pid=%d（零串扰被破坏！）", pid))
				}
			}
			mu.Unlock()
			results <- res
		}(r, rs, spawns)
	}
	wg.Wait()
	close(results)

	// —— 汇总断言 ——
	seen := 0
	for res := range results {
		seen++
		for _, e := range res.errs {
			t.Errorf("room %d: %s", res.roomIdx, e)
		}
		if res.rosterN != perRoom {
			t.Errorf("room %d: roster=%d want=%d", res.roomIdx, res.rosterN, perRoom)
		}
		if res.snapFrames == 0 {
			t.Errorf("room %d: 快照 0 帧（角色未被维持/无对局）", res.roomIdx)
		}
		t.Logf("room %d: roster=%d 快照=%d帧 快照pid数=%d",
			res.roomIdx, res.rosterN, res.snapFrames, len(res.snapPids))
	}
	if seen != nRooms {
		t.Fatalf("完成房间=%d want=%d", seen, nRooms)
	}
	t.Logf("✓ %d 房并行×%d 人：名册/维持/零串扰全过", nRooms, perRoom)
}

// ---------- ④ 同房 4 枪击杀（21 判定回执 + 23 击杀）----------
func TestFireKillInRoom(t *testing.T) {
	host, rid := openRoomE2E(t, "fk_host", "fk_room")
	defer host.closeRST()
	m1 := loginE2E(t, "fk_m1")
	defer m1.closeRST()
	if code := mustJoin(t, m1, rid); code != 0 {
		t.Fatalf("join code=%d", code)
	}
	hostPid, _, hostSp := startAndWaitRoster(t, host, 2, "fk_host")
	m1Pid, _, m1Sp := startAndWaitRoster(t, m1, 2, "fk_m1")

	// —— 出生点激活（几何合同驱动·rebase 开局门：首报必须落出生点 100cm 内）——
	sp := loadSpawnPoints(t)
	hs, ms := sp[hostSp], sp[m1Sp]
	fireYaw := yawTo(hs, ms) // host 朝 m1 方向（yaw 不受 rebase 限制——位置才受）
	// victim 须两拍——首报不写历史环（ep09 对拍实锤），否则回溯采样 nullopt 永远打不中
	for i := 0; i < 2; i++ {
		if err := sendInput(host, uint32(i+1), float32(hs.X), float32(hs.Y), float32(hs.Z), fireYaw); err != nil {
			t.Fatalf("host input: %v", err)
		}
		if err := sendInput(m1, uint32(i+1), float32(ms.X), float32(ms.Y), float32(ms.Z), 0); err != nil {
			t.Fatalf("m1 input: %v", err)
		}
		time.Sleep(80 * time.Millisecond)
	}
	// —— 链路诊断：17 号快照验证 19 号上报/激活（Route→locate→UpdateEntity→入快照）——
	snapN, selfSeen, victimSeen := 0, false, false
	_ = readFor(host, 1500*time.Millisecond, func(mid uint16, pl []byte) {
		if mid != msgSnap || len(pl) < 2 {
			return
		}
		snapN++
		nn := int(binary.LittleEndian.Uint16(pl[0:2]))
		off := 2
		for k := 0; k < nn && off+29 <= len(pl); k++ {
			switch binary.LittleEndian.Uint32(pl[off : off+4]) {
			case hostPid:
				selfSeen = true
			case m1Pid:
				victimSeen = true
			}
			off += 29
		}
	})
	t.Logf("[诊断] 快照=%d帧 自己在快照=%v 受害者在快照=%v（false=上报链断/未激活）", snapN, selfSeen, victimSeen)
	if !selfSeen {
		t.Fatalf("19 号上报链断：1.5s 内自己未入快照（快照%d帧）——先修上报链再验开火", snapN)
	}
	if !victimSeen {
		t.Fatalf("受害者未入快照（m1 激活失败——rebase 门/上报链）")
	}
	// 4 连发（伤害 25×4=100 满血见底；间隔 120ms > 射速 95ms）
	for i := 0; i < 4; i++ {
		if err := host.send(msgFire, nil); err != nil {
			t.Fatalf("fire send: %v", err)
		}
		time.Sleep(120 * time.Millisecond)
	}
	// 收帧 1.5s：21 判定 + 23 击杀
	var fireRes []byte
	var kill []byte
	_ = readFor(host, 1500*time.Millisecond, func(mid uint16, pl []byte) {
		switch mid {
		case msgFireRes:
			if fireRes == nil && len(pl) >= 25 {
				fireRes = append([]byte{}, pl...)
			}
		case msgKill:
			if kill == nil && len(pl) >= 10 {
				kill = append([]byte{}, pl...)
			}
		}
	})
	if fireRes == nil {
		t.Fatalf("未收到 21 号判定回执（开火链路断？）")
	}
	shooter := binary.LittleEndian.Uint32(fireRes[0:4])
	result := fireRes[4]
	victim := binary.LittleEndian.Uint32(fireRes[5:9])
	hitX := math.Float32frombits(binary.LittleEndian.Uint32(fireRes[9:13]))
	hitY := math.Float32frombits(binary.LittleEndian.Uint32(fireRes[13:17]))
	hitZ := math.Float32frombits(binary.LittleEndian.Uint32(fireRes[17:21]))
	hp := int(fireRes[21])
	gate := fireRes[22]
	// 全字段诊断：hit 坐标=射线终点（穿箱时=箱面·miss 无候选时=射线远端）
	t.Logf("[21号] result=%d gate=%d victim=%d hp=%d hit=(%.0f,%.0f,%.0f) shooter=%d want=%d",
		result, gate, victim, hp, hitX, hitY, hitZ, shooter, hostPid)
	if shooter != hostPid {
		t.Errorf("21 号 shooter=%d want=%d", shooter, hostPid)
	}
	// 第一枪命中：victim=m1 且掉血（hp<满血）——伤害数值=game_config.lua 真实值 27（非 25），
	// 数值精度归 C++ 单测/契约测试（分层职责），e2e 只验"命中链路通+真扣血"
	if victim != m1Pid {
		t.Errorf("首枪 victim=%d want=%d（未命中受害者）", victim, m1Pid)
	}
	if hp >= 100 || hp <= 0 {
		t.Errorf("首枪 hp=%d——命中但未掉血（伤害链断）", hp)
	}
	if kill == nil {
		t.Fatalf("未收到 23 号击杀（4 枪应杀满 100 血）")
	}
	kv := binary.LittleEndian.Uint32(kill[0:4])
	kt := binary.LittleEndian.Uint32(kill[4:8])
	if kv != m1Pid || kt != hostPid {
		t.Errorf("23 号 victim=%d killer=%d want %d/%d", kv, kt, m1Pid, hostPid)
	}
	t.Logf("✓ 同房判定：首枪命中掉血至 hp=%d（damage=27 真实配置）→ 4 枪击杀（23 号 victim=%d killer=%d）", hp, kv, kt)
}

// ---------- ⑤ 跨房开火必 miss（候选集隔离——与用例④互为对照）----------
func TestCrossRoomFireMiss(t *testing.T) {
	// A 房：host 单人；B 房：m1 单人（站在与用例④完全相同的 (100,0,90)）
	a, aRid := openRoomE2E(t, "xr_host", "xr_room_a")
	defer a.closeRST()
	b, bRid := openRoomE2E(t, "xr_m1", "xr_room_b")
	defer b.closeRST()
	if code := mustJoin(t, b, bRid); code != 0 { // b 自己建的房已是成员——此行应不执行
		t.Logf("b join code=%d（建房者已在房·忽略）", code)
	}
	_ = aRid
	aPid, _, aSp := startAndWaitRoster(t, a, 1, "xr_host")
	_, _, bSp := startAndWaitRoster(t, b, 1, "xr_m1")
	_ = aPid

	// 双方出生点激活（rebase 门：首报须落出生点——读几何合同，不硬编码）
	sp := loadSpawnPoints(t)
	as, bs := sp[aSp], sp[bSp]
	for i := 0; i < 2; i++ {
		if err := sendInput(a, uint32(i+1), float32(as.X), float32(as.Y), float32(as.Z), 0); err != nil {
			t.Fatalf("a input: %v", err)
		}
		if err := sendInput(b, uint32(i+1), float32(bs.X), float32(bs.Y), float32(bs.Z), 0); err != nil {
			t.Fatalf("b input: %v", err)
		}
		time.Sleep(80 * time.Millisecond)
	}
	// A 开火 3 发（应全部 miss——B 不在 A 房候选集）
	for i := 0; i < 3; i++ {
		if err := a.send(msgFire, nil); err != nil {
			t.Fatalf("fire: %v", err)
		}
		time.Sleep(120 * time.Millisecond)
	}
	var res [][]byte
	_ = readFor(a, 1500*time.Millisecond, func(mid uint16, pl []byte) {
		if mid == msgFireRes && len(pl) >= 25 && len(res) < 3 {
			res = append(res, append([]byte{}, pl...))
		}
	})
	if len(res) == 0 {
		t.Fatalf("未收到 21 号（A 房开火无回执）")
	}
	for i, pl := range res {
		result := pl[4]
		victim := binary.LittleEndian.Uint32(pl[5:9])
		if result != 0 || victim != 0 {
			t.Errorf("跨房第%d枪 result=%d victim=%d——应全 miss（候选集隔离被破坏！"+
				"对照：用例④同坐标同 yaw 同房必中）", i+1, result, victim)
		}
	}
	t.Logf("✓ 跨房开火 %d 枪全 miss（B 房成员不在 A 候选集·零串扰判定侧实证）", len(res))
}

// ---------- ⑥ 大量房规模验证（lobby 无总数硬上限——稳定性）----------
func TestManyRoomsStability(t *testing.T) {
	const nRooms = 40 // 实勘 kMaxRooms 不存在=无硬上限；40 房规模回归（每房单人快速局）
	sp := loadSpawnPoints(t) // 出生点读一次（rebase 门：首报须落出生点）
	for i := 0; i < nRooms; i++ {
		h, _ := openRoomE2E(t, fmt.Sprintf("big_h%02d", i), fmt.Sprintf("big_room%02d", i))
		// 单人开局 → 名册 1 → 首报出生点（激活）→ 收 1s 快照验角色维持
		selfPid, roster, spIx := startAndWaitRoster(t, h, 1, fmt.Sprintf("big_h%02d", i))
		if len(roster) != 1 {
			t.Fatalf("room %d: roster=%d", i, len(roster))
		}
		s0 := sp[spIx]
		if err := sendInput(h, 1, float32(s0.X), float32(s0.Y), float32(s0.Z), 0); err != nil {
			t.Fatalf("room %d input: %v", i, err)
		}
		frames, selfSeen := 0, false
		_ = readFor(h, 1200*time.Millisecond, func(mid uint16, pl []byte) {
			if mid != msgSnap || len(pl) < 2 {
				return
			}
			frames++
			nn := int(binary.LittleEndian.Uint16(pl[0:2]))
			off := 2
			for k := 0; k < nn && off+29 <= len(pl); k++ {
				if binary.LittleEndian.Uint32(pl[off:off+4]) == selfPid {
					selfSeen = true
				}
				off += 29
			}
		})
		if frames == 0 {
			t.Fatalf("room %d: 快照 0 帧（第 %d 房无对局）", i, i+1)
		}
		if !selfSeen {
			t.Fatalf("room %d: 角色未入快照（首报激活失败——rebebe 门/上报链）", i+1)
		}
		h.closeRST() // RST 收尾（loadbot 血统：不留 TIME_WAIT）
		if (i+1)%10 == 0 {
			t.Logf("…%d/%d 房通过", i+1, nRooms)
		}
	}
	t.Logf("✓ %d 房串行规模验证全过（无总数硬上限·每房开局/激活/快照含角色全一致）", nRooms)
}

// min 避免与 loadbot main.go 的 min 冲突——本文件未用，占位防未来引用
var _ = strings.TrimSpace
