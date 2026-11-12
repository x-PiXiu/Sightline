// account_e2e_test.go —— ep09 步骤 6（自动化）：账号域**真库闭环**验收
// 形态：自起 full 模式实例（:18897·连面板 MySQL 127.0.0.1:3306/sightline）→ Go 模拟客户端
//   发 31/32/34 号 → 收 33/35 号回执 → **exec mysql 查 t_account 行**（注册→SQL 闭环=演示镜头之锚）
// 前置：MySQL 在跑 + init.sql 已执行（t_account 表——幂等脚本，本测试不建表不清库）
// 跑法：~/go/bin/go test -v -timeout 120s -run TestAccountE2E .
//   （服务器不可用/DB 不可达 → 测试失败即步骤 6 未完成信号）
package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"
)

const (
	fullAddr   = "127.0.0.1:18897"
	msgAccountReg   = 31
	msgAccountLogin = 32
	msgAuthResult   = 33
	msgQueryStats   = 34
	msgStatsResult  = 35
)

// authConn：注册/登录（31/32 同构帧）→ 等收 33 号 → 解析
func authReq(t *testing.T, c *conn, kind uint16, account, pass string) (ok bool, reason byte, accountId uint32, mode uint8, name string) {
	t.Helper()
	pl := []byte{byte(len(account))}
	pl = append(pl, account...)
	pl = append(pl, byte(len(pass)))
	pl = append(pl, pass...)
	if err := c.send(kind, pl); err != nil {
		t.Fatalf("auth send: %v", err)
	}
	deadline := time.Now().Add(8 * time.Second)
	for {
		mid, payload, err := c.readFrame(deadline)
		if err != nil {
			t.Fatalf("auth 等回执(33): %v", err)
		}
		if mid != msgAuthResult {
			continue // 跳过旁路（15/17/30 等）
		}
		if len(payload) < 8 {
			t.Fatalf("33 号过短 len=%d", len(payload))
		}
		ok = payload[0] != 0
		reason = payload[1]
		accountId = binary.LittleEndian.Uint32(payload[2:6])
		mode = payload[6]
		nl := int(payload[7])
		if len(payload) >= 8+nl {
			name = string(payload[8 : 8+nl])
		}
		return
	}
}

// queryStats：34 号 → 等 35 号 → rows
func queryStats(t *testing.T, c *conn, limit uint8) int {
	t.Helper()
	if err := c.send(msgQueryStats, []byte{limit}); err != nil {
		t.Fatalf("query send: %v", err)
	}
	deadline := time.Now().Add(8 * time.Second)
	for {
		mid, payload, err := c.readFrame(deadline)
		if err != nil {
			t.Fatalf("等 35 号: %v", err)
		}
		if mid != msgStatsResult {
			continue
		}
		if len(payload) < 1 {
			t.Fatalf("35 号过短")
		}
		return int(payload[0])
	}
}

// sqlCount：exec mysql 查 t_account 匹配行数（真库闭环断言）——密码警告走 stderr 丢弃
func sqlCount(t *testing.T, name string) int {
	t.Helper()
	cmd := exec.Command("mysql", "-h127.0.0.1", "-P3306", "-usightline",
		"-psightline", "sightline", "-N", "-e",
		fmt.Sprintf("SELECT COUNT(*) FROM t_account WHERE name='%s'", name))
	var out, errOut strings.Builder
	cmd.Stdout = &out
	cmd.Stderr = &errOut
	if err := cmd.Run(); err != nil {
		t.Fatalf("mysql exec: %v errOut=%s", err, errOut.String())
	}
	s := strings.TrimSpace(out.String())
	var n int
	if _, err := fmt.Sscanf(s, "%d", &n); err != nil {
		t.Fatalf("mysql 输出非数字: %q", s)
	}
	return n
}

func TestAccountE2E(t *testing.T) {
	// —— 自起 full 实例（:18897·连真库）——
	srv := exec.Command("../../sightline_server", "--db-profile=full") // 显式 full（默认 lite 无账号域）
	srv.Env = append(os.Environ(),
		"SIGHTLINE_PORT=18897",
		"SIGHTLINE_CMD_PORT=0",
		"SIGHTLINE_MAP_GEOMETRY=/nonexistent_e2e.json",
	)
	srv.Stdout = os.Stdout
	srv.Stderr = os.Stderr
	if err := srv.Start(); err != nil {
		t.Fatalf("full 实例启动失败: %v", err)
	}
	defer func() {
		_ = srv.Process.Signal(syscall.SIGTERM)
		done := make(chan struct{})
		go func() { _ = srv.Wait(); close(done) }()
		select {
		case <-done:
		case <-time.After(6 * time.Second):
			_ = srv.Process.Kill()
		}
	}()
	ready := false
	for i := 0; i < 20; i++ {
		if c, err := net.DialTimeout("tcp", fullAddr, time.Second); err == nil {
			_ = c.Close()
			ready = true
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if !ready {
		t.Fatalf("full 实例 10s 未就绪 :%s", fullAddr)
	}

	acct := fmt.Sprintf("e2e_%x", time.Now().UnixNano()%0xFFFFF) // 唯一（连跑不撞）
	const pass = "e2e_secret1"

	// —— ① 注册 → 33 ok=1·accountId>0·mode=1 ——
	// 31 号不要求先游客登录（注册不需要 pid）——直连即发（WELCOME 旁路由 authReq 跳过）
	ac, err := dial(fullAddr)
	if err != nil {
		t.Fatalf("dial full: %v", err)
	}
	defer ac.closeRST()
	ok, reason, aid, mode, name := authReq(t, ac, msgAccountReg, acct, pass)
	if !ok || reason != 0 || aid == 0 || mode != 1 || name != acct {
		t.Fatalf("注册回执异常: ok=%v reason=%d aid=%d mode=%d name=%q", ok, reason, aid, mode, name)
	}
	t.Logf("✓ 注册：accountId=%d mode=%d", aid, mode)

	// —— ② 重复注册 → reason=1 ——
	ok, reason, _, _, _ = authReq(t, ac, msgAccountReg, acct, pass)
	if ok || reason != 1 {
		t.Fatalf("重复注册: ok=%v reason=%d want reason=1", ok, reason)
	}
	t.Logf("✓ 重复注册拒 reason=1")

	// —— ③ 错密码登录 → reason=3 ——
	ok, reason, _, _, _ = authReq(t, ac, msgAccountLogin, acct, "wrongpass!")
	if ok || reason != 3 {
		t.Fatalf("错密码: ok=%v reason=%d want reason=3", ok, reason)
	}
	t.Logf("✓ 错密码拒 reason=3")

	// —— ④ 正确登录 → ok=1 ——
	ok, reason, aid2, mode2, _ := authReq(t, ac, msgAccountLogin, acct, pass)
	if !ok || reason != 0 || aid2 != aid || mode2 != 1 {
		t.Fatalf("登录: ok=%v reason=%d aid=%d(want %d) mode=%d", ok, reason, aid2, aid, mode2)
	}
	t.Logf("✓ 登录：accountId=%d（与注册一致）", aid2)

	// —— ⑤ 战绩查询 → 35 号回执（新号 0 行——链路通即验证）——
	n := queryStats(t, ac, 10)
	t.Logf("✓ 战绩查询回执 rows=%d（新号 0 行=预期）", n)

	// —— ⑥ 真库闭环：SQL 查 t_account 行（注册→SQL 闭环=演示镜头之锚）——
	if n := sqlCount(t, acct); n != 1 {
		t.Fatalf("t_account 中 %q 行数=%d want=1", acct, n)
	}
	t.Logf("✓ SQL 闭环：t_account 中已见 %q 行", acct)

	// —— ⑦ 资源清理 ——
	ac.closeRST()
}
