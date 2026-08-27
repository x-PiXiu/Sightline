# scripts/ 操作手册

> 按功能分四类目录：**build（构建与质量）/ verify（功能验证）/ bench（压测）/ debug（调试取证）**。
> 另有独立工具：`loadbot/`（Go 协议压测 bot，见第五节）。
> 通用约定：脚本可从任意目录调用（自动定位项目根）；WSL/Linux 环境；"自带服"= 脚本自起实例并自动清理，"外部服"= 压测已运行实例。

---

## 一、build/ 构建与质量（改完代码必跑）

### `build/build_and_test.sh` —— 一键编译 + 全部单测
```bash
wsl bash scripts/build/build_and_test.sh
```
- 做什么：`cmake + make -j`（Release）→ `ctest` 三套单测（hitscan / room / room_service）
- 期望：`100% tests passed, 0 tests failed out of 3`

### `build/check_dependencies.sh` —— 架构依赖守护（三红线）
```bash
wsl bash scripts/build/check_dependencies.sh
```
- 检查：domain 净空 / application 净空 / net 不含 game 头
- 期望：三条 `✓ 干净`；可挂 CI

---

## 二、verify/ 功能验证（自带服）

### `verify/run_smoke.sh` —— 冒烟测试（断言版）
```bash
wsl bash scripts/verify/run_smoke.sh        # 自带服，端口 8891
```
- 双客户端走 登录→进房→开战→移动→命中→RTT 全流程，逐步断言；期望 `SMOKE TEST: ALL PASSED`

### `verify/run_demo.sh` —— 交互演示（字节级可视化）
```bash
wsl bash scripts/verify/run_demo.sh [port]  # 自带服，默认 8889
```
- 七步对战全流程，每条消息打印 hex + 解析（配 docs/字节协议与传输详解.md）
- `verify/demo_interact.py` 可单独对运行中实例执行：`python3 scripts/verify/demo_interact.py 8888`

---

## 三、bench/ 压测

### `bench/bench_client.py` —— Python 压测客户端（快速档）
```bash
python3 scripts/bench/bench_client.py --port 8888 --n 1000 --hz 5 --duration 25 --ramp 300
```
- 参数：`--n` 并发 / `--hz` Move 频率 / `--duration` 秒 / `--ramp` 建连速率
- 适用：≤5000 连接的快速验证；RTT 含 asyncio 客户端开销（偏高），精确延迟用 loadbot

### `bench/run_bench.sh` —— 标准三阶段（自带服，Release）
```bash
wsl bash scripts/bench/run_bench.sh [P1] [P2] [P3]   # 默认 500/1000/2000
```
- 每阶段采集 RSS/CPU/fd + 服务器 stats；基线：2000 连接 ≈24KB/连接

### `bench/run_bench_5k.sh` —— 容量极限（自带服）
```bash
wsl bash scripts/bench/run_bench_5k.sh [n] [port]    # 默认 5000
```

### `bench/run_bench_ext.sh` —— 压测外部实例（不启停目标）
```bash
wsl bash scripts/bench/run_bench_ext.sh <port> <pid>
```
- 四阶段 500/1000/2000/5000；⚠️ 目标进程自身 fd 上限决定容量（CLion 启动默认 1024 → ~1013 并发封顶）

### `bench/run_bench_go.sh` —— Go loadbot 压测（重型档，推荐）
```bash
wsl bash scripts/bench/run_bench_go.sh <n> <hz> <dur> [port]
# 例：wsl bash scripts/bench/run_bench_go.sh 10000 1 30
```
- 自带服 + Go bot（真并发调度）；实测基线：**10000 连接全成功、上行 9332/s、RTT p50 0.2ms、8KB/连接**

---

## 四、debug/ 调试取证（升级顺序：最小复现 → 日志 → 系统调用 → 内存检测）

| 脚本 | 用法 | 时机 |
|------|------|------|
| `probe_login.sh` | `wsl bash scripts/debug/probe_login.sh` | 怀疑基础链路不通的第一步（单条 Login + hex 回显） |
| `repro_crash.sh` | `wsl bash scripts/debug/repro_crash.sh` | 进程无声死亡时 gdb 起服 + 断线风暴，崩溃自动打全线程栈（需 `apt install gdb`） |
| `repro_asan.sh` | `wsl bash scripts/debug/repro_asan.sh` | 怀疑悬空指针/越界：ASan 构建 + 10 轮流量风暴，不崩也能抓 UAF |
| `probe_strace.sh` | （需先装 strace） | epoll 问题看内核真相的模板 |
| `bench_disconnect_storm.py` | `python3 scripts/debug/bench_disconnect_storm.py 300 8888` | 纯 RST 断线风暴注入（SIGPIPE 就是它抓出来的） |
| `bench_crash_v2.py` | `python3 scripts/debug/bench_crash_v2.py 8888 400 4` | 流量+风暴组合、10 轮循环，复现时序型崩溃 |

---

## 五、loadbot/ —— Go 协议压测 bot（重型压测正解）

**为什么自写**：通用工具无法编码游戏协议与行为（登录→进房→心跳）；Go 的 goroutine-per-conn 一天写出 5 万并发压测端——Python asyncio 单线程在 ~5000 连接处饱和，且其调度开销污染 RTT 测量（同负载下 Python 报 p50 16.6ms，Go 实测 0.3ms，50 倍差距）。

```bash
# 构建（WSL 内，Go 工具链装在 ~/go）
cd loadbot && ~/go/bin/go build -o loadbot .
# 直连运行中的服务器
./loadbot/loadbot -host 127.0.0.1 -port 8888 -n 5000 -hz 2 -dur 30 -ramp 2000 -ping 2
```
- 行为与 Python 版同协议同语义；每连接独立 goroutine（读写分离），Pong 测 RTT，5s 进度播报
- 已实测：10000 连接 0 失败 / 9332 up + 14232 down msg/s / p50 0.2ms / 82MB
- 已知观察：万连接爬坡期 p99 有数百 ms 尖刺（疑同步日志逐条刷 + 建连日志风暴），p50/p90 不受影响——列为下一步优化项

---

## 附：压测课速记（全部踩过）

1. 服务器与压测端 `ulimit -n` 各自独立；CLion 图形启动 = 会话默认 1024；
2. 客户端"带未读数据关闭"= RST 注入（故意逼 SIGPIPE 类问题）；
3. 客户端"连接成功"≠ 服务器 accept（内核 backlog 先握手），以下行消息计数为准；
4. 同机 loopback 测逻辑容量，RTT 须标注拓扑与压测端类型（Python/Go 差 50 倍）。
