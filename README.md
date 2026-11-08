# Sightline

企业级缩小版多人 FPS：**自研 Linux C++ 服务器 + UE5 客户端**，开发全程视频公开记录。

> 一家不存在的公司，一份真实的工作。——《领导请批示 · 开发周报》

## 系列视频

| 期 | 视频 | 代码快照 | 本期交付 | 关键数字 |
|----|------|----------|----------|----------|
| 序章 · 一条心跳的两种命运 | [BV113WF6MEBZ](https://www.bilibili.com/video/BV113WF6MEBZ/) | — | 接手前任网络库，五个隐患逐个拆 | — |
| 第 1 期 · UE 连上服务器 | [BV113WF6ME8f](https://www.bilibili.com/video/BV113WF6ME8f/) | `ep01` | 心跳 / ping / LAG 重连，UE 首连 | 登录 213ms，心跳往返 62–228ms |
| 第 2 期 · 登录 + 房间系统 | [BV1SNWF6GEuk](https://www.bilibili.com/video/BV1SNWF6GEuk/) | `ep02` | 登录 / 房间 / 顶号 / 协议契约 | 回环响应 <1ms；8 类非法请求全拦 |
| 第 3 期 · 移动同步 + 网络滑块 | [BV1NYn56zEyk](https://www.bilibili.com/video/BV1NYn56zEyk/) | `ep03` | 朴素移动同步，损伤滑块 0~300ms | 30Hz 上报每包 21B |
| 第 4 期 · 预测 · 和解 · 插值 | [BV1X3nY6iEfE](https://www.bilibili.com/video/BV1X3nY6iEfE/) | `ep04` | 三部曲 + 主从 Reactor + 压测 | 3 万同时在线全绿；单测 125 断言 |
| 第 5 期 · hitscan 与服务器权威命中 | [BV1PHxN69EFf](https://www.bilibili.com/video/BV1PHxN69EFf/) | `ep05` | 20 号空包 + 四道门 + 解析求交 + 贴花管理器 | 全链 RTT 45ms（判定 1ms）；单测 160 断言 |
| 第 6 期 · 延迟补偿回溯 | [BV1u8x46ZE7R](https://www.bilibili.com/video/BV1u8x46ZE7R/) | `ep06` | 历史环 32 槽 + judgeAt=now−rtt + 22 号上报 + 幽灵三色 | 回溯关 21.4%→开 39.7%（滑块 300ms）；单测 97 断言 |
| 第 7 期 · 死亡与结算 | [BV16C4h6tEZe](https://www.bilibili.com/video/BV16C4h6tEZe/) | `ep07` | 服务器权威死亡/重生/结算 + 人机三件套 + 比分板 + 死窗根除（扣弹即上钟） | 一局完整对局 ≈85s；死窗 38/147 发→0；单测 238 断言 |
| 第 8 期 · 不停服调平衡 | 待发布 | `ep08` | 配置热重载 + Lua 公式/决策热修 + 三库数据层（MySQL/Mongo/Redis·故障降级）+ 动画表驱动换枪 + bot_ai 切 Lua + 双模式部署 | 单测 587 断言；双模式对拍逐字节一致；Lumen 根除 Frame 83→17.3ms |

每期代码快照 = 对应 tag，可 `git checkout epNN` 复现当期状态。

## 架构

- **服务端** `server/`：epoll 主从 Reactor（IO 多线程 + 逻辑单线程），定时器为纯最小堆 HeapScheduler；整洁架构四层——domain / application / adapters / net，业务层不碰网络细节。
- **协议**：TCP `[2B len][2B msgid][payload]` 小端；契约登记在 `docs/protocol/MSG_IDS.md`（msgid 只增不改），第 4 期起 variant 编解码（每条消息一个专用解析器，格式不对整条拒收）。
- **客户端** `client/`：UE5.7，C++ 为主，蓝图只做表现层；客户端预测 + 服务器权威对账（ε=5cm 三分支）+ 远端插值（滑窗 + 25ms 渲染延迟 + 三层滤波）。表现层素材来自 Epic 官方 Lyra（不入库，见 `client/README.md`）。

## 快速开始

### 1. 依赖安装（Ubuntu 22.04 / WSL 实测）

```bash
# 编译工具链 + 协议/存储/Lua 开发库（探测式构建：装齐即自动启用 Lua 策略层与三库持久化）
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
    liblua5.4-dev libhiredis-dev libmariadb-dev libmongoc-1.0-dev

# 运行时三件套（full 模式用；不装也能跑——lite 模式零依赖启动，见下）
sudo apt install -y mysql-server redis-server          # MongoDB 建议官方源装 mongodb-org（apt 默认源无）
```

### 2. 构建

```bash
git clone https://github.com/x-PiXiu/Sightline.git
cd Sightline/server
cmake -B build && cmake --build build -j$(nproc)
# 未装 Lua/DB 开发库也能 configure+build（对应功能自动关闭）；
# 显式控制：-DSIGHTLINE_ENABLE_LUA=ON / -DSIGHTLINE_ENABLE_DB=ON
```

### 3. 启动（双模式）

```bash
# lite 模式（默认·零依赖裸跑——无任何数据库即可完整开局）
./build/sightline_server

# full 模式（三库持久化：MySQL 战绩+审计链 / Redis 热榜 / MongoDB 战报文档）
sudo systemctl start mysql redis-server mongod          # 或使用宝塔/Docker 托管实例
mysql -h127.0.0.1 -uroot -p < assets/db/init.sql        # 建库建表（幂等，可重复执行）
export SIGHTLINE_MYSQL_DSN='127.0.0.1:3306:sightline:sightline:sightline'
export SIGHTLINE_REDIS_URI='127.0.0.1:6379:<你的Redis密码>'
export SIGHTLINE_MONGO_URI='mongodb://127.0.0.1:27017'
./build/sightline_server --db-profile=full
# 启动横幅：[DB] profile=full · MySQL ✓ · Mongo ✓ · Redis ✓ —— 任一库缺失自动降级，对局照常
```

### 4. 运维命令口（与日志流分离）

```bash
nc 127.0.0.1 18889        # 连上后敲：version / reload [note] / rollback / patch damage / help
```

### 5. 单元测试（8 组 · 587 项断言）

```bash
cd server && ./build.sh all          # 一键构建+跑全部测试（脚本自动探测 Lua/DB 库）
./build/test_lobby    # 45 断言 · LobbyService
./build/test_match    # 238 断言 · MatchService 全规则
./build/test_wire     # 26 断言 · 协议字节级布局
./build/test_ports    # 42 断言 · 端口与纯策略
./build/test_config   # 48 断言 · 配置热重载
./build/test_lua_policy  # 91 断言 · Lua 沙箱/热修/等价性对拍（需 Lua）
./build/test_persistence # 28 断言 · 持久化/WAL
./build/test_v48      # 69 断言 · 契约 v4.8（换枪/热重载三语义）
SIGHTLINE_DB_SMOKE=1 /tmp/test_db_integration   # 真库集成冒烟（可选·需三库在跑）
```

### 6. 客户端

UE5.7 工程（`client/`），`.uproject` 与 Lyra 素材获取见 `client/README.md`。

## 压测（loadbot v2 · 2026-10-23）

- **30,004 并发连接全绿**（0 失败，82.9s）——容量层为 30000 大厅长连接（登录/心跳/RTT 流量）；
  另有独立对局压测档：真实对局全流程（30Hz 输入上报、快照 30.3 帧/s、ack 滞后 ≤1）。
  两档分离压测——容量与对局逻辑的真实负载各自见底，不是"只连不打"的混测。
- 50k 探墙实测撞 `somaxconn=1024` 内核队列（46,100 登录），属环境天花板非服务器上限。
- loadbot 独立仓库脚本，goroutine-per-conn，三模式（大厅 / 对局 / 混合），多目标 IP 轮转绕单 IP 临时端口墙。

## 仓库结构

```
server/          # C++ 服务器：src/{net,lobby,match,logger,protocol} + tests/ + scripts/loadbot
client/          # UE5 客户端工程（源码入库，Lyra 素材与 .uasset 不入库）
docs/
  protocol/      # MSG_IDS.md 协议契约（服务端与 UE 的共同契约，变更必须双端同步）
  episodes/      # 每期定稿架构图（.drawio + .png）、设计草案、验收数据
  architecture/  # 全局架构图
```

## 工具链

AI 实习生：Zcode｜架构手册：[architect-handbook](https://skillhub.cn)｜UE 素材：Epic 官方 Lyra（只取素材）｜前任的遗产（毕设网络库）：[x-PiXiu/MicroserviceDemo](https://github.com/x-PiXiu/MicroserviceDemo)
