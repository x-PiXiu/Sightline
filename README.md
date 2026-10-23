# Sightline

企业级缩小版多人 FPS：**自研 Linux C++ 服务器 + UE5 客户端**，开发全程视频公开记录。

> 一家不存在的公司，一份真实的工作。——《领导请批示 · 开发周报》

## 系列视频

| 期 | 视频 | 代码快照 | 本期交付 | 关键数字 |
|----|------|----------|----------|----------|
| 序章 · 一条心跳的两种命运 | [BV113WF6MEBZ](https://www.bilibili.com/video/BV113WF6MEBZ/) | — | 接手前任网络库，五个隐患逐个拆 | — |
| 第 1 期 · UE 连上服务器 | [BV113WF6ME8f](https://www.bilibili.com/video/BV113WF6ME8f/) | `ep01` | 心跳 / ping / LAG 重连，UE 首连 | 登录 213ms，心跳往返 62–228ms |
| 第 2 期 · 登录 + 房间系统 | [BV1SNWF6GEuk](https://www.bilibili.com/video/BV1SNWF6GEuk/) | `ep02` | 登录 / 房间 / 顶号 / 协议契约 | 回环响应 <1ms；8 类非法请求全拦 |
| 第 3 期 · 移动同步 + 网络滑块 | 定时发布中（2026-10-24） | `ep03` | 朴素移动同步，损伤滑块 0~300ms | 30Hz 上报每包 21B |
| 第 4 期 · 预测 · 和解 · 插值 | 待定时 | `ep04` | 三部曲 + 主从 Reactor + 压测 | 3 万同时在线全绿；单测 125 断言 |

每期代码快照 = 对应 tag，可 `git checkout epNN` 复现当期状态。

## 架构

- **服务端** `server/`：epoll 主从 Reactor（IO 多线程 + 逻辑单线程），定时器为纯最小堆 HeapScheduler；整洁架构四层——domain / application / adapters / net，业务层不碰网络细节。
- **协议**：TCP `[2B len][2B msgid][payload]` 小端；契约登记在 `docs/protocol/MSG_IDS.md`（msgid 只增不改），第 4 期起 variant 编解码（每条消息一个专用解析器，格式不对整条拒收）。
- **客户端** `client/`：UE5.7，C++ 为主，蓝图只做表现层；客户端预测 + 服务器权威对账（ε=5cm 三分支）+ 远端插值（滑窗 + 25ms 渲染延迟 + 三层滤波）。表现层素材来自 Epic 官方 Lyra（不入库，见 `client/README.md`）。

## 构建与测试

```bash
# 服务端（Linux / WSL）
cd server && cmake -B build && cmake --build build -j

# 单元测试（125 项断言）
./build/test_lobby    # 45 断言 · LobbyService 纯逻辑
./build/test_match    # 54 断言 · MatchService + 对账桥
g++ -std=c++2b -I include tests/test_wire.cpp -o test_wire && ./test_wire   # 26 断言 · 协议字节级布局

# 压测（见 server/scripts/loadbot/）
cd server/build && ulimit -n 65535 && SIGHTLINE_PORT=18889 ./sightline_server
cd server/scripts/loadbot && ./loadbot_linux -port 18889 -n 30000 -matchN 4 -ramp 600 -mramp 100 -hold 15
```

客户端为 UE 工程，`.uproject` 与 Content 资产按 `client/README.md` 说明获取 Lyra 素材后构建。

## 压测（loadbot v2 · 2026-10-23）

- **30,004 并发连接全绿**（0 失败，82.9s），且每 4 人一房进入真实对局：30Hz 输入上报、快照 30.3 帧/s、ack 滞后 ≤1——不是只连不玩。
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
