# Sightline 项目交接与 UE 客户端实现指南

> **读者**：在另一台机器上接手本项目的 Agent（或人）。
> **目标**：15 分钟建立项目全貌 → 按步骤实现配套 UE 客户端并与服务器联调。
> **本文原则**：所有事实以本文和 `docs/` 两份文档为准，不依赖对话历史。

---

## 第一部分：项目快速上下文（Agent 必读卡）

### 1.1 这是什么

**Sightline**（视线即弹道）：C++17 手写 epoll 的 FPS 对战游戏服务器（Linux/WSL），按整洁架构分层。配套一个待实现的 UE5 蓝图+C++ 客户端。项目由毕设网络库改造而来，是求职作品集（面试叙事核心：手写 Reactor + 整洁架构 + 3 万连接压测 + 五个调试侦探故事）。

### 1.2 一句话架构

```
main.cpp（组装工）
  └─ adapters/   GameServer·ProtocolCodec·TcpConnection·Acceptor   ← 翻译与分发
       └─ application/   SessionService·RoomService + ports/(2个接口) ← 用例编排
            └─ domain/   Room状态机·Player·hitscan纯函数                ← 零IO依赖规则
  net/ + logger/   毕带网络库（epoll/EventLoop/HeapScheduler）         ← 框架层
```

**铁律**：依赖只向内。domain 不 include 任何东西（标准库除外）；application 只依赖 domain+自有 ports；net 不含任何 game 头。守护脚本：`bash scripts/build/check_dependencies.sh`（必须全绿）。

### 1.3 关键事实卡

| 项 | 值 |
|---|---|
| 语言/平台 | C++17 / Linux（epoll/eventfd/timerfd，**只能 WSL 或 Linux 跑**） |
| 构建 | `bash scripts/build/build_and_test.sh`（编译+4套单测） |
| 冒烟 | `bash scripts/verify/run_smoke.sh`（自带服8891，期望 SMOKE TEST: ALL PASSED） |
| 长驻启动 | `bash scripts/serve.sh 8888 [debug]`（日志落 `logs/server-8888.log`） |
| 端口 | 默认 8888（用户调试实例常用）/ 8891 冒烟 / 8899+ 压测 |
| 协议 | `[2B len][2B msgid][payload]` 小端；14 条消息（msgid 1~14，契约见下表） |
| 心跳 | 客户端 ≤8 秒任意消息喂狗（推荐 2 秒一次 Ping msgid=10），超时服务器踢人 |
| 环境 | WSL 内 GCC 13+/CMake 3.16+；代码已含 IWYU 修复（GCC 15 可编译，commit `0d0f723`） |

### 1.4 当前状态（截至 commit `0d0f723`）

**已完成并验证**：
- 完整对战闭环：登录→进房(两两自动开战)→移动同步→服务器权威 hitscan 命中→击杀→重生(3s)→胜负(3杀)→断线判负→重开一局(5s)
- 主从 Reactor（IO 多线程+逻辑单线程）、优雅关闭、高水位踢人、令牌桶限速（默认关）
- 异步日志+限频聚合、周期 stats（含 timerfd_settime 计数）
- 4/4 单测、冒烟全通、压测：30000 连接 0 失败、p50 0.4ms、≈8KB/连接

**已知待做（按优先级）**：
1. **UE 客户端联调**（本文第二部分 = 就是要做的事）
2. soak 1 小时长跑；剩余 p99 尖刺归属（服务器端自测打点）
3. KCP 传输层（adapters 层换 TcpConnection 实现，业务零改动）

### 1.5 必读文档（按需查阅，不要全读）

| 文档 | 什么时候读 |
|---|---|
| `docs/架构设计文档.md` | 要改架构/分层/加新层时（含 11 步消息流转图） |
| `docs/字节协议与传输详解.md` | **实现 UE 客户端前必读**（帧格式逐字节+消息表） |
| 知识库 `学习路线/UE蓝图与自研服务器通讯指南.md` | 本文 UE 部分的原理底稿（C++薄封装完整代码） |

### 1.6 Agent 常用操作

```bash
# 构建+测试（任何改动后）
wsl bash scripts/build/build_and_test.sh
# 起服务（长驻，日志落盘）
wsl bash scripts/serve.sh 8888 debug        # debug=显示连接进出
# 实时看日志
tail -f /e/workspace/Demo/C_Demo/Sightline/logs/server-8888.log   # Git Bash
# 对运行中的 8888 跑七步演示（验证服务器健康）
wsl bash scripts/verify/demo_interact.py 8888
# 依赖守护（架构红线）
wsl bash scripts/build/check_dependencies.sh
```

---

## 第二部分：设计初衷浓缩（Agent 需要的"为什么"）

1. **为什么手写 epoll 不用现成框架**：求职叙事需要证明网络功底；毕设库（50000 行微服务）剥离出 3813 行网络库，"三次进化"（毕设→生产支付→游戏服）。
2. **为什么整洁架构**：业务逻辑可脱离网络单测（4 套测试零 socket）；网络层可整体替换（KCP 计划）；**已验证的收益**：异步日志单变量实验 p99 258 倍改善时，业务层一行未改。
3. **为什么只有 2 个接口**（IGameChannel/ITimerScheduler，即将 +1 IRuleEngine）：接口只在"需要第二实现或测试替身"时建——反过度设计红线，毕设 800 行配置热更新是前车之鉴。
4. **为什么 IO 多线程+逻辑单线程**：游戏状态强共享，多线程=锁地狱；IO 并行搬运字节、逻辑串行处理消息（queueInLoop 跳回主 loop），跨线程只传纯数据。
5. **为什么 TCP 不是 UDP**：规模决策（8人房10Hz 队头阻塞不可见），传输层已隔离，KCP 是既定演进。

---

## 第三部分：UE 客户端逐步实现（核心任务）

> 总原则：**UE 只做表现层**——Socket/拆包归 C++，HUD/动画归蓝图。服务器已在 8888 运行且用 `demo_interact.py` 验证过健康，UE 是第二个"客户端"。
> 建议里程碑：M1 裸连+文字日志（本机双开）→ M2 移动同步互见 → M3 开火命中扣血 → M4 打磨 UI。每步有验收标准。

### Step 0：环境准备（约 30 分钟）

1. Windows 侧安装 **Visual Studio 2022**（勾选"使用 C++ 的游戏开发"工作负载，含 UE 安装器组件）
2. 通过 Epic 启动器安装 **UE 5.3+**
3. 确认服务器在跑：`wsl bash scripts/serve.sh 8888`，然后 `wsl bash scripts/verify/demo_interact.py 8888` 全绿
4. **WSL2 网络确认**：Windows 侧 `Test-NetConnection 127.0.0.1 -Port 8888` 应为 True（WSL2 默认 localhost 转发）。若不通，用 WSL 内 `hostname -I` 的 IP 直连

### Step 1：创建 UE 工程（10 分钟）

1. Epic 启动器 → 新建项目：**Games → Blank → C++**（务必选 C++，不是 Blueprint）→ 项目名 `SightlineClient` → 创建
2. 创建后 VS 会自动打开，先按 F5 空编译一次确认工具链正常
3. **设置 Net Mode 为 Standalone**（关键！）：Project Settings → Maps & Modes 不动即可；Play 下拉箭头 → Net Mode 选 **Standalone Game**——我们用裸 socket，绝不能让 UE 自己的网络栈掺和
4. PIE 多开：Play 按钮旁下拉 → Number of Players = 2（后续双端联调用）

### Step 2：C++ 薄封装——网络子系统（核心，约 1.5 小时）

新建两个文件（工程 Source/SightlineClient/ 下）：

**`SightlineNetSubsystem.h`** —— 完整代码（可直接抄）：

```cpp
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "SightlineNetSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNetMessage, int32, MsgId, const TArray<uint8>&, Payload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDisconnected);

UCLASS(BlueprintType)
class SIGHTLINECLIENT_API USightlineNetSubsystem : public UGameInstanceSubsystem,
                                                  public FTickableGameObject
{
    GENERATED_BODY()
public:
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override
    { RETURN_QUICK_DECLARE_CYCLE_STAT(USightlineNetSubsystem, STATGROUP_Tickables); }

    UFUNCTION(BlueprintCallable, Category="Sightline|Net")
    bool Connect(const FString& Ip, int32 Port);

    UFUNCTION(BlueprintCallable, Category="Sightline|Net")
    void Disconnect();

    // 发原始帧：内部组装 [2B len][2B msgid][payload]
    UFUNCTION(BlueprintCallable, Category="Sightline|Net")
    bool SendFrame(int32 MsgId, const TArray<uint8>& Payload);

    UPROPERTY(BlueprintAssignable) FOnNetMessage OnNetMessage;
    UPROPERTY(BlueprintAssignable) FOnDisconnected OnDisconnected;

private:
    void PumpRecv();          // ET 语义：循环读到 EAGAIN
    void ExtractPackets();    // 长度前缀拆包 → 广播事件
    bool SendRaw(const uint8* Data, int32 Size);

    FSocket* Socket = nullptr;
    bool bConnected = false;
    TArray<uint8> Accum;      // 累积缓冲（粘包/半包）
};
```

**`SightlineNetSubsystem.cpp`** —— 关键实现：

```cpp
#include "SightlineNetSubsystem.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

bool USightlineNetSubsystem::Connect(const FString& Ip, int32 Port)
{
    Disconnect();
    ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORMSUBSYSTEM);
    Socket = SS->CreateSocket(NAME_Stream, TEXT("Sightline"), false);
    if (!Socket) return false;

    TSharedPtr<FInternetAddr> Addr = SS->CreateInternetAddr(*Ip, Port);
    // 本地/局域网直连：阻塞连接毫秒级完成，成功后再切非阻塞
    if (!Socket->Connect(*Addr)) { SS->DestroySocket(Socket); Socket=nullptr; return false; }
    Socket->SetNonBlocking(true);
    Socket->SetNoDelay(true);        // FPS 必关 Nagle
    bConnected = true;
    return true;
}

void USightlineNetSubsystem::Disconnect()
{
    if (Socket) { Socket->Close(); ISocketSubsystem::Get(PLATFORMSUBSYSTEM)->DestroySocket(Socket); Socket=nullptr; }
    bConnected = false; Accum.Empty();
}

void USightlineNetSubsystem::Tick(float)
{
    if (!bConnected || !Socket) return;
    PumpRecv();
    ExtractPackets();
}

void USightlineNetSubsystem::PumpRecv()
{
    uint8 Tmp[4096]; int32 Read = 0;
    while (Socket->Recv(Tmp, sizeof(Tmp), Read, ESocketReceiveFlags::NonBlocking))
    {
        if (Read <= 0) break;                 // 0=对端关闭，非阻塞无数据也返回false
        Accum.Append(Tmp, Read);
        if (Read < (int32)sizeof(Tmp)) break; // 读干了
    }
}

void USightlineNetSubsystem::ExtractPackets()
{
    // 协议：[2B len][2B msgid][payload]，小端（UE Windows 目标机天然小端）
    while (Accum.Num() >= 4)
    {
        uint16 Len = Accum[0] | (Accum[1] << 8);
        uint16 MsgId = Accum[2] | (Accum[3] << 8);
        if (Accum.Num() < 4 + Len) break;     // 半包：等下一帧
        OnNetMessage.Broadcast(MsgId,
            TArray<uint8>(Accum.GetData() + 4, Len));
        Accum.RemoveAt(0, 4 + Len);           // 消费一条（粘包继续循环）
    }
}

bool USightlineNetSubsystem::SendFrame(int32 MsgId, const TArray<uint8>& Payload)
{
    if (!bConnected || Payload.Num() > 0xFFFF) return false;
    TArray<uint8> Pkt;
    uint16 Len = (uint16)Payload.Num();
    Pkt.Add((uint8)(Len & 0xFF));  Pkt.Add((uint8)(Len >> 8));
    Pkt.Add((uint8)(MsgId & 0xFF)); Pkt.Add((uint8)(MsgId >> 8));
    Pkt.Append(Payload);
    int32 Sent = 0;
    return Socket->Send(Pkt.GetData(), Pkt.Num(), Sent) && Sent == Pkt.Num();
}

bool USightlineNetSubsystem::SendRaw(const uint8* Data, int32 Size)
{ int32 Sent=0; return Socket && Socket->Send(Data, Size, Sent) && Sent==Size; }

void USightlineNetSubsystem::Deinitialize()
{ Disconnect(); Super::Deinitialize(); }
```

**验收**：编译通过（Ctrl+Shift+B），无报错。

### Step 3：协议辅助库（30 分钟）

新建 `SightlineProto.h`（静态工具类，编解码全部消息）：

```cpp
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SightlineProto.generated.h"

// 消息 ID（与服务器 protocol_codec.h 的 MsgId 枚举严格一致，改动需两端同步！）
UENUM(BlueprintType)
enum class EMsg : uint8
{
    C2S_Login=1, S2C_LoginAck=2, C2S_JoinRoom=3, S2C_RoomStart=4,
    C2S_Move=5, S2C_Move=6, C2S_Fire=7, S2C_Hit=8, S2C_GameOver=9,
    C2S_Ping=10, S2C_Pong=11, S2C_Kick=12, S2C_PlayerLeft=13, S2C_Respawn=14
};

UCLASS()
class SIGHTLINECLIENT_API USightlineProto : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // ---- 编码（C→S）----
    static TArray<uint8> EncLogin(const FString& Name) {
        FTCHARToUTF8 Conv(*Name);
        TArray<uint8> P; uint16 Len = (uint16)Conv.Length();
        P.Add(Len & 0xFF); P.Add(Len >> 8);
        P.Append((uint8*)Conv.Get(), Conv.Length());
        return P;
    }
    static TArray<uint8> EncMove(FVector Pos, float Yaw) {
        TArray<uint8> P; float V[4] = { Pos.X, Pos.Z, Pos.Y, Yaw }; // UE是左手系Y-up：x,z,y映射见下注
        for (int i = 0; i < 4; ++i) {
            uint32 Bits; FMemory::Memcpy(&Bits, &V[i], 4);
            P.Append({(uint8)Bits, (uint8)(Bits>>8), (uint8)(Bits>>16), (uint8)(Bits>>24)});
        }
        return P;
    }
    static TArray<uint8> EncFire(FVector Origin, FVector Dir) {
        TArray<uint8> P;
        FVector D[2] = { Origin, Dir };
        for (auto& V : D) for (int i = 0; i < 3; ++i) {
            float Comp = (i==0)?V.X : (i==1)?V.Z : V.Y;   // 同上坐标映射
            uint32 Bits; FMemory::Memcpy(&Bits, &Comp, 4);
            P.Append({(uint8)Bits, (uint8)(Bits>>8), (uint8)(Bits>>16), (uint8)(Bits>>24)});
        }
        return P;
    }
    static TArray<uint8> EncPing(uint64 ClientTimeMs) {
        TArray<uint8> P;
        for (int i = 0; i < 8; ++i) P.Add((uint8)(ClientTimeMs >> (8*i)));
        return P;
    }

    // ---- 解码辅助（S→C，蓝图也能用）----
    UFUNCTION(BlueprintPure, Category="Sightline|Proto")
    static int32 DecodeU32(const TArray<uint8>& B, int32 Offset) {
        if (B.Num() < Offset + 4) return 0;
        return B[Offset] | (B[Offset+1]<<8) | (B[Offset+2]<<16) | (B[Offset+3]<<24);
    }
    UFUNCTION(BlueprintPure, Category="Sightline|Proto")
    static float DecodeF32(const TArray<uint8>& B, int32 Offset) {
        if (B.Num() < Offset + 4) return 0.f;
        uint32 Bits = DecodeU32(B, Offset); float V;
        FMemory::Memcpy(&V, &Bits, 4); return V;
    }
};
```

> **坐标系注意**：服务器是 y-up 右手系数学上无所谓（服务器只存数值并原样广播），**关键是 UE 侧自己保持一致**：上报什么、收回来就插值什么。上面的 X/Z↔X/Y 映射写法是常见坑位——最简单做法是**不映射**，把服务器的 (x,y,z) 直接当 UE 的 (X,Y,Z) 用，出生点在地面上改 Z 即可。M1 里程碑先用不映射版本。

### Step 4：蓝图接线——M1 裸连里程碑（30 分钟）

1. 打开关卡蓝图（或建一个 `BP_NetDemo` Actor 放进关卡）
2. BeginPlay：
   - `Get Game Instance Subsystem`（选 SightlineNetSubsystem）
   - `Connect`（IP=127.0.0.1，Port=8888）
   - 成功 → `SendFrame`（MsgId=1 即 C2S_Login，Payload 用 EncLogin("UEPlayer")）
   - 绑定 `OnNetMessage` 事件
3. OnNetMessage 事件里：`Switch on Int` (MsgId) → MsgId==2 分支用 `DecodeU32(Payload, 0)` 取 playerId → `AddOnScreenDebugMessage` 显示（M1 不做 UMG）
4. 加一个 2 秒定时器发 Ping（MsgId=10，Payload=当前毫秒），收到 11 号显示 RTT

**M1 验收**：PIE 运行 → 屏幕显示"playerId=1"和滚动的 RTT；服务器日志（`tail -f logs/server-8888.log`）出现 `conn#N in`。

### Step 5：移动同步——M2 里程碑（1 小时）

1. 新建 `BP_RemotePawn`（基于 Pawn，加一个 Cube 静态网格）
2. 收到 S2C_Move（msgid=6）：解析 `[4B pid][3×4F pos][4F yaw]` → 按 pid 存 `TargetPos` 到 Map
3. RemotePawn 的 Tick：`SetActorLocation(FMath::VInterpTo(Current, TargetPos, DeltaTime, 8.0))` —— **这就是插值**，10Hz 快照画出 60FPS 动画
4. 本地玩家：默认 Pawn 随便动（WASD），用**10Hz 定时器**读当前位置发 C2S_Move
5. 双开验证：Play 下拉 → Number of Players=2（NetMode 保持 Standalone）→ 两窗口各动各的，互相能看到对方平滑移动

**M2 验收**：双 PIE 实例，窗口 A 动，窗口 B 里 A 的方块平滑跟随（有 ~100ms 观察延迟属正常——这就是插值缓冲）。

### Step 6：开火与命中——M3 里程碑（1 小时）

1. 本地开火（鼠标左键 Action 绑定）：
   - 立即本地播枪口特效（表现不等网络——客户端预测思想）
   - 发 C2S_Fire（msgid=7）：origin=相机位置，dir=相机前向单位向量
2. 收到 S2C_Hit（msgid=8）：解析 shooter/victim/dmg/hp/dead
   - 我是 victim → 屏幕红闪 + 扣血
   - 我是 shooter 且 dead → 显示"击杀"
3. 被击杀后 3 秒服务器自动发 S2C_Respawn（msgid=14）→ 把本地 Pawn 位置设为 respawn 点、血量复位
4. 收到 S2C_GameOver（msgid=9）→ 显示"Winner: N"大字

**M3 验收**：双开对射，血量按服务器判定扣减；4 枪击杀 → 3 秒重生 → 再战。

### Step 7：打磨（可选，各 30 分钟）

- UMG 替换 OnScreenDebugMessage：HP 条、RTT、击杀信息
- Kick（msgid=12）→ 弹"你被踢了（心跳超时）"→ 断线重连按钮
- PlayerLeft（13）/RoomStart（4）→ 进出房提示
- 断线检测：子系统 Tick 里检查 socket 状态 → 自动重连按钮

### Step 8：联调 Checklist 与常见坑

| 坑 | 症状 | 解法 |
|----|------|------|
| WSL2 端口不通 | UE 连接超时 | 用 WSL `hostname -I` 的 IP；或确认 `.wslconfig` localhostForwarding |
| 消息收到但字段乱 | 数字巨大/为 0 | 字节序或字段偏移错——对照服务器 `protocol_codec.h` 逐字段核对 |
| UE 端自己弹网络错误 | PIE 报 NetDriver 错 | Net Mode 没设 Standalone |
| 服务器 8 秒踢人 | 踢掉后收 Kick(12) | Ping 定时器没启动/间隔 >8s |
| 双开互看不到 | 各自正常但不互见 | 没都发 JoinRoom(3)，或房间人满（max_players=4） |
| 移动瞬移不平滑 | 远端方块跳变 | 忘了插值，直接 SetActorLocation(TargetPos) |

### Step 9：交付标准（Definition of Done）

- [ ] 双 PIE 实例完整对局：登录→互见移动→对射→击杀→重生→胜负→自动重开一局
- [ ] HUD 显示：playerId、HP、RTT、击杀播报
- [ ] 拔服务器（kill 进程）→ UE 端触发断线提示，不崩溃
- [ ] 服务器日志无 ERROR 级噪音（连接/断开均为预期路径）
- [ ] 录一段双端对战视频（面试演示素材，放 `docs/` 或仓库外）

---

## 第四部分：给 Agent 的提醒

1. **不要动 domain/application 层来"配合"客户端**——客户端适配服务器，不是反过来。需要新消息时在 `protocol_codec.h` 加 MsgId，两端同步。
2. **改动后必跑**：`build_and_test.sh` + `check_dependencies.sh`，全绿才算完成。
3. **UE 工程不要放进 Sightline 仓库**（体积差两个数量级），建议放 `E:\workspace\Demo\SightlineClient\`，在本文档记录路径即可。
4. **做完每个里程碑就 git commit + push**——这台机器和另一台机器靠 GitHub 同步。
5. 服务器"健康"的判定标准：`demo_interact.py` 七步全绿。客户端联调出问题先跑它排除服务器侧。
