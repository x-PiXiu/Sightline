# HUD 实现手册（P3 表现打磨）

> **定位**：P1 收官后的下一阶段——把 Print 全部替换成 UMG 界面，达成交付标准的"HUD 显示：playerId、HP、RTT、击杀播报"。
> **前提**：P1 已完成（M1/M2/M3 回归通过）；C++ 数据出口全部就绪（含本阶段新增的流量统计）。
> **服务端**：零改动。
> **工作量**：M4.1~M4.5 合计约 2~2.5 小时，每个子阶段独立验收。

---

## 一、设计原则（动手前必读）

### 1.1 数据流：事件驱动为主，HUD 永远不碰网络

```
C++ 事件（已有,8 个 BP_On* 钩子）              HUD（WBP_HUD）
────────────────────────────                 ──────────────
BP_OnLoggedIn(playerId)          ──推──►     PlayerId 文本
BP_OnRttUpdated(rtt)             ──推──►     RTT 文本
BP_OnHitReceived(...,NewHp,...)  ──推──►     HP 条 + 受击闪红
BP_OnRespawned(pos,hp)           ──推──►     HP 条回满 + 遮罩关闭
BP_OnKillConfirmed(victim)       ──推──►     击杀播报（滚动一条）
BP_OnGameOver(winner,bIWon)      ──推──►     结算大字
BP_OnKicked / BP_OnDisconnected ──推──►     断线提示
```

- **HUD 不订阅子系统、不解析协议、不知道服务器存在**——所有数据经 BP_Player（C++ 钩子）推送；
- 弹药是唯一例外（数据源在 Weapon 上）：**HUD Tick 低频轮询** `GetCurrentWeapon → AmmoInMag`（60fps 读两个 int 开销可忽略，零新增管线）；
- 这就是《03-客户端架构》"C++ 管状态、蓝图管表现"在 UI 层的落地：**C++ 推数据，UMG 只画**。

### 1.2 C++ 数据出口清单（全部已就绪，无需改代码）

| 数据 | 出口 | 类型 |
|---|---|---|
| playerId | `GetPlayerId()` | BlueprintPure int32 |
| HP | `GetHP()` / 钩子参数 NewHp | BlueprintPure int32 |
| RTT | `BP_OnRttUpdated(rtt)` 推送 | int64 毫秒 |
| 弹药 | `GetCurrentWeapon() → AmmoInMag / ReserveAmmo / MagSize` | BlueprintPure int32 |
| 死亡状态 | `IsDead()` / 钩子参数 bDead | BlueprintPure bool |
| 流量 | `GetNet() → GetBytesSent / GetBytesReceived` | BlueprintPure int64 |
| 胜负 | `BP_OnGameOver(winner, bIWon)` | — |

---

## 二、WBP_HUD 控件结构（一次性搭好）

新建 `Content/Code/UI/WBP_HUD`（UserWidget），Canvas Panel 下：

```
WBP_HUD (UserWidget)
├── Canvas
│   ├── Crosshair（准星）        ← 屏幕正中，图片/十字线，M4.1 顺带
│   ├── TopLeft（左上角信息块）
│   │   ├── PlayerIdText        ← "Player 3"
│   │   └── RTTText             ← "RTT: 27ms"
│   ├── BottomLeft（左下角）
│   │   ├── HPBar（Progress Bar）← 0~1
│   │   └── HPText              ← "75 / 100"
│   ├── BottomRight（右下角）
│   │   ├── AmmoText            ← "27 / 180"
│   │   └── WeaponText          ← "VIRTUS"（可省）
│   ├── KillFeed（击杀播报，右上角下方）
│   │   └── 用 Vertical Box + 动态添加 Text（M4.4）
│   ├── DeathOverlay（死亡遮罩，全屏，默认 Hidden）
│   │   └── 半透明黑 + "你被击杀，3 秒后重生..." 文本
│   └── NetPanel（Tab 网络面板，全屏半透明，默认 Hidden）
│       ├── SentText            ← "↑ 12.3 KB/s"
│       ├── RecvText            ← "↓ 20.1 KB/s"
│       └── （RTT 复用 TopLeft 的值）
```

**操作提示**：Palette 搜 Progress Bar / Text Block 拖入；锚点用各角落的锚点预设（左上信息块锚左上，弹药锚右下）；文本用字体大小 14~18。

---

## 三、分阶段实现

### M4.1 HUD 骨架 + PlayerId + RTT + 准星（30 分钟）

**BP_Player 事件图**（Event BeginPlay 的**最前面**，先于其他逻辑）：

```
Event BeginPlay
    │
    ▼
Create Widget (Class = WBP_HUD)          ← 返回 HUD 引用
    │
    ▼
Add to Viewport (ZOrder = 10)
    │
    ▼
提升变量 "HudRef"（WBP_HUD 引用，供所有钩子使用）
    │
    ▼
[原有逻辑...]
```

**BP_OnLoggedIn 覆盖**（替换 Print）：

```
Event BP_OnLoggedIn (NewPlayerId)
    │
    ▼
HudRef → SetPlayerIdText("Player " + ToString)
```

**BP_OnRttUpdated 覆盖**（替换 Print）：

```
Event BP_OnRttUpdated (RttMs)
    │
    ▼
HudRef → SetRttText("RTT: " + ToString + "ms")
```

> WBP_HUD 里为每个可更新元素写一个 **Set 函数**（如 SetPlayerIdText/SetRttText/SetHP）——
> 蓝图钩子只调函数，不直接摸控件内部（控件层级变了钩子不用改）。

**验证**：PIE → 屏幕常显 PlayerId + RTT 滚动 + 中央准星；Output Log 不再出现对应 Print。

### M4.2 HP 条 + 死亡遮罩（30 分钟）

**数据**：`BP_OnHitReceived(Shooter, Damage, NewHp, bDead)` 与 `BP_OnRespawned(Pos, Hp)` 推送；HUD 内部存 `MaxHp = 100`（服务器 RoomRules 常量，协议不传——写注释注明）。

**WBP_HUD 加函数**：

```
SetHP (NewHp:int32):
    HPBar → Set Percent (NewHp / MaxHp)
    HPText → Set Text ("{NewHp} / 100")
    （可选：NewHp < 30 时 HPBar 颜色切红）

ShowDeath (Visible:bool):
    DeathOverlay → Set Visibility (Visible / Collapsed)
```

**BP_Player 覆盖接线**：

```
BP_OnHitReceived → HudRef → SetHP(NewHp)
                  → Branch(bDead) → True: HudRef → ShowDeath(true)
BP_OnRespawned   → HudRef → SetHP(Hp) + ShowDeath(false)
```

**验证**：对射 → HP 条递减、红闪可选 → 被打死全屏遮罩 → 3 秒后重生条回满、遮罩消失。

### M4.3 弹药显示（30 分钟，唯一轮询项）

**WBP_HUD Event Tick**（低频处理：累积 0.1s 才刷一次，避免每帧 SetText）：

```
Event Tick
    │
    ▼
Get Owning Player Pawn → Cast BP_Player
    │
    ▼
Get Current Weapon → 有效？
    │True
    ▼
Ammo = Weapon → Ammo In Mag ；Reserve = Weapon → Reserve Ammo
    │
    ▼
与上次缓存值不同才 Set Text ("{Ammo} / {Reserve}")   ← 缓存避免每帧重绘
```

**验证**：开火弹匣递减、空仓自动换弹后回满——数字与服务器行为一致。

### M4.4 击杀播报（20 分钟）

**WBP_HUD 加函数** `AddKillFeed(VictimId:int32)`：

```
Create Widget (WBP_KillEntry)     ← 一条小文本控件："你击杀了 Player {VictimId}"
    │
    ▼
KillFeed(Vertical Box) → Add Child
    │
    ▼
Set Timer by Function (3 秒后 Remove From Parent)   ← 播报 3 秒消失
```

**BP_Player 的 `BP_OnKillConfirmed(Victim)`** → `HudRef → AddKillFeed(Victim)`。

### M4.5 Tab 网络面板（30 分钟）

**输入**：可复用现有 IMC 加一个 IA_NetworkPanel（Tab 键，Pressed 显示 / Released 隐藏——按住查看模式），或最简做法：BP_Player 里 `Event BeginPlay` 后用 `Input` 节点绑定 Tab。

**WBP_HUD 加函数**：

```
ToggleNetPanel():
    NetPanel 可见性切换 (Visible ↔ Collapsed)

SetNetTraffic(Sent:int64, Recv:int64):   ← 由 HUD Tick 每 0.5s 调用
    SentText → "↑ " + (Sent - LastSent)/0.5/1024 + " KB/s"   ← 差分算速率
    RecvText → 同理
```

**数据源**：`GetNet() → GetBytesSent / GetBytesReceived`（C++ 刚加的，累计字节数；HUD 差分算 KB/s）。

**验证**：按住 Tab → 面板显示，速率数字稳定在合理范围（双方 10Hz Move ≈ 下行 ~1.6KB/s 上行 ~0.8KB/s 每实例，参考服务端 stats）。

---

## 四、完成后清理

- [ ] BP_Player 事件图里 8 个旧 Print 全部删除（被 HUD 取代）
- [ ] `Output Log` 里不再有 Gameplay 打印噪音
- [ ] 全流程回归：M1（登录进房）→ M2（互见）→ M3（对射闭环），全程看 HUD 作战
- [ ] commit + push（含 WBP_HUD / WBP_KillEntry / BP_Player / BP_Weapon 的改动）

**DoD 对标**（交接文档 Step 9）：HUD 显示 playerId、HP、RTT、击杀播报 ✓ → 进入演示视频可录状态。

---

## 五、常见问题

| 症状 | 原因 | 解法 |
|---|---|---|
| HUD 不显示 | CreateWidget 后忘了 AddToViewport，或 ZOrder 被盖 | 检查两步都连了；ZOrder=10 |
| HP 条不动 | 钩子没覆盖 / HudRef 变量没赋值（CreateWidget 之后才提升） | 确认提升变量在 CreateWidget 之后 |
| 弹药永远 0/0 | GetCurrentWeapon 为空 | Default Weapon Class 没配，或武器未装备 |
| Tick 里 Cast 失败 | Owning Player Pawn 不是 BP_Player | GameMode 的 Default Pawn Class 检查 |
| 两个 PIE 窗口 HUD 互串 | HUD 加进了不属于自己的 viewport | CreateWidget 外层套 GetOwningPlayerPawn 相关判断（一般不会发生，Standalone 各自独立） |
| 速率显示 0 | 差分基准没存（LastSent/LastRecv 变量） | HUD 里加两个 int64 缓存变量 |

---

## 六、为什么这样设计（面试可讲的点）

1. **事件驱动推送 + 单一轮询例外**：状态变更（HP/RTT/击杀）走 C++ 钩子推送，UI 永远是"被通知方"；唯一轮询的弹药是刻意的取舍——数据源在武器上，跨 Actor 事件管线成本 > 每 0.1s 读两个 int。**性能敏感的轮询必有缓存差分**（值变了才 SetText）。
2. **HUD 零网络感知**：WBP_HUD 不知道 Subsystem/协议存在——它只认 BP_Player 的接口。换传输层/协议版本，HUD 一行不改。
3. **Set 函数封装控件**：蓝图钩子调 `HUD.SetHP(75)` 而非直接摸 Progress Bar——控件树重构不波及调用方。
4. **流量统计下沉 C++**：字节计数在收发路径上原子累加（SendFrame/PumpRecv），UI 只读——计数与展示解耦，这与服务端 `GameServer::logStats` 的统计设计互为镜像。
