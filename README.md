# Sightline

> **视线即弹道**（Sight = 视线，Line = 弹道）——一个 hitscan FPS 对战服务器的名字，
> 也是它的核心玩法逻辑：开火瞬间沿视线做射线判定。

C++17 手写 epoll 的 FPS 对战游戏服务器（Linux）。由毕设网络库改造而来，按整洁架构分层。

> **架构详解**：分层设计、消息流转全景图、开火/心跳/重生三条核心调用链路、断线重连场景推演
> → 见 [`docs/架构设计文档.md`](docs/架构设计文档.md)（面试讲解的完整底稿）
>
> **字节协议详解**：帧格式逐字节解剖（含真实抓包）、粘包半包、ET 读写路径、协议演进
> → 见 [`docs/字节协议与传输详解.md`](docs/字节协议与传输详解.md)（配 `scripts/verify/run_demo.sh` 交互演示）

```
┌──────────────────────────────────────────────────┐
│ main.cpp                组装工：装配、注入、上电   │
├──────────────────────────────────────────────────┤
│ adapters/  接口适配层                              │
│   TcpConnection + Acceptor + Buffer   连接与字节  │
│   ProtocolCodec                       字节 ↔ DTO  │
│   GameServer        消息分发 + IGameChannel 实现   │
│   TimerWheelAdapter ITimerScheduler 实现          │
├──────────────────────────────────────────────────┤
│ application/  用例层                               │
│   SessionService  登录/心跳/超时踢人               │
│   RoomService     进房/移动/开火编排/胜负/重生     │
│   ports/          IGameChannel + ITimerScheduler  │
├──────────────────────────────────────────────────┤
│ domain/  实体层（零依赖，可纯单测）                │
│   Room 房间状态机 · Player · combat/ hitscan+伤害  │
├──────────────────────────────────────────────────┤
│ net/ + logger/  框架与驱动层（毕带网络库，通用）   │
│   Epoll·Channel·EventLoop·Socket·分层时间轮       │
└──────────────────────────────────────────────────┘
        依赖方向：一律向内（main → adapters → application → domain ← net 不含任何 game 头）
```

## 构建与运行

```bash
# Linux / WSL
mkdir build && cd build
cmake .. && make -j
./sightline [port]           # 默认 8888
ctest                        # 跑全部单测（无需网络环境）
bash ../scripts/build/check_dependencies.sh   # 依赖方向守护（三条红线）
```

## 协议

帧格式：`[2B payload_len][2B msg_id][payload]`（小端），消息 ID 1~14，
与 UE 客户端实现共同契约：详见知识库《UE蓝图与自研服务器通讯指南》第五节消息表。

## 单测说明（架构的体检报告）

| 测试 | 覆盖 | 为什么不需要网络 |
|------|------|------------------|
| test_hitscan | 射线×AABB：正打/擦过/身后/取最近/斜向 | 纯数学函数 |
| test_room | 状态机、击杀链、三杀获胜、退房判胜、满员 | 纯实体规则 |
| test_room_service | 登录→进房→开战→命中→踢人→判胜 全流程 | Fake 通道 + Fake 定时器替身 |

## 对《毕设服务器改造分析》三份文档的执行偏差（诚实记录）

1. **thread_pool 未迁移**（原计划"带走但不接入"）：它对 ConfigManager 有深层依赖
   （ThreadPoolConfig/热更新），而单线程逻辑服务器用不到——按 YAGNI 裁掉，
   需要时从原仓库迁移并去 config 化。原仓库不受影响。
2. **IBroadcaster 更名为 IGameChannel**：接口同时承担发送与主动断开（心跳踢人），
   "Channel"更名实。
3. **服务从三个合并为两个**：MatchService 并入 RoomService（进房即匹配），
   CombatService 的实体逻辑在 domain::Room::applyFire 中——避免薄转发层。
4. **C2S_Move 线上格式不含玩家 ID**：身份由会话上下文推导，客户端无法伪造他人
   移动（服务器权威的安全细节）。
5. **Channel 补了 tie() 生命周期守卫**（muduo 标配，毕设版缺失）：防止事件回调中
   连接销毁导致 Channel 悬空——新写的 TcpConnection 依赖它保证安全销毁。

## Roadmap

> 详细演进规划（每阶段的业务触发/服务端变化/客户端变化/契约面变更/验收标准）见 [`docs/演进路线图.md`](docs/演进路线图.md)。

- [x] P0：服务器基线（整洁架构 + 自研 Reactor + M1~M3 协议 + 压测）
- [ ] P1：蓝图从零搭建 + UE 客户端联调（手册见 `docs/client/05`）
- [ ] P2：道具系统（NetID/TypeID + 数据驱动练兵）
- [ ] P3：HUD/表现打磨（UMG）
- [ ] P4：夺旗战（IRoomMode 多模式抽取）
- [ ] P5：断线重连
- [ ] P6：客户端预测与服务器校正
- [ ] P7：KCP 传输层
- [ ] P8：protobuf + 数据驱动武器
- [ ] P9：武器配件系统
- [ ] P10：Soak 长跑压测
