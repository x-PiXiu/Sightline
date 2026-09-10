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

## 二、WBP_HUD 控件结构搭建（详细步骤）

### 2.0 术语速查：HUD / UMG / 控件蓝图的关系

**HUD 是"要画的东西"（战斗信息层），UMG 是"画笔"（UE 的 UI 编辑系统），WBP_HUD 是画布（控件蓝图资产）。**

| 编辑器里的名字 | 对应概念 |
|---|---|
| UMG 编辑器 | 双击控件蓝图打开的三栏界面（层级树 / 画布 / Details） |
| 画布面板（Canvas Panel） | 根容器——绝对定位基准，所有元素的父级 |
| 控件（Text/ProgressBar/Overlay…） | 画布上的元素 |
| 锚点（Anchor） | 控件"钉在屏幕哪个角"的基准（分辨率变化时位置不变的关键） |

### 2.1 创建控件蓝图 + 添加画布

1. 内容浏览器进入 `Content/Code/UI/` → 空白处右键 → **用户界面** → **控件蓝图** → 父类选 **UserWidget**（默认）→ 命名 `WBP_HUD`
2. 双击打开——**UE5 新建的设计器默认是空的，没有画布**：
   - 中间空白区有快捷创建按钮 → 直接点 **画布面板（Canvas Panel）**
   - 没看到按钮就在左下 **Palette（面板）** 搜 `画布` → 拖进层级面板空白区
3. 画布自动铺满设计器——这是根，之后所有控件都拖到它下面

> **简化说明**：下面结构树里的 TopLeft/BottomLeft 是**概念分组**，实际不用建容器——把控件直接放 Canvas 下、各自锚到对应角即可。

### 2.2 逐控件搭建（统一三步：拖入 → F2 改名 → Details 设置）

**通用操作**：
- **拖入**：左下 Palette 搜索控件名 → 拖到层级树 Canvas 下
- **改名**：层级树选中 → **F2**
- **锚点**：选中控件 → Details 顶部的 **画布面板** 分类 → **锚点** 下拉（九宫格图标）→ 选预设 → 填位置数值
- **内容/外观**：Details 的 内容（文字）/ 外观（字体、颜色）分类

| Palette 搜索 | 改名 | 锚点预设 | 关键属性 |
|---|---|---|---|
| 文本 | `PlayerIdText` | 左上 (20,20) | 内容 `Player ?`；字体 18 |
| 文本 | `RTTText` | 左上 (20,55) | 内容 `RTT: --`；字体 16 |
| 进度条 | `HPBar` | 左下 (20,-60) | 百分比 1.0；填充颜色绿色；高 20 |
| 文本 | `HPText` | 左下 (20,-30) | 内容 `100 / 100`；字体 16 |
| 文本 | `AmmoText` | 右下 (-160,-60) | 内容 `30 / 180`；字体 18 |
| 文本 | `WeaponText` | 右下 (-160,-30) | 内容 `VIRTUS`；字体 12 |
| 文本 | `Crosshair` | **居中** | 内容 `+`；字体 24；颜色白（没有准星图的占位方案） |
| 垂直框 | `KillFeed` | 右上 (-300,80)，宽 280 | 空容器（M4.4 动态填充） |

### 2.3 DeathOverlay（死亡遮罩）——含子组件与默认隐藏

1. Palette 搜 `覆盖层`（Overlay）→ 拖到 Canvas 下 → F2 改名 `DeathOverlay`
2. 选中它 → Details → 画布面板分类 → 锚点选**铺满**（九宫格中央撑满预设）→ 位置四边全部 0
3. **往 Overlay 里拖一个 `图像`** → 改名 `DeathBackdrop`：
   - Details → **外观** → **笔刷** → **颜色**：点开取色器 → R/G/B = 0（黑）、**A = 0.6**（半透明）
   - 无贴图即为纯色块；槽位对齐保持**填充** → 自动铺满全屏
4. **再拖一个 `文本` 进 Overlay** → 改名 `DeathText`：
   - 内容 `你被击杀，3 秒后重生…`；字体 32
   - 外观 → 对齐 → 水平/垂直 = 居中
5. **选中 `DeathOverlay` 本身** → Details → **行为** → **可见性** → **已折叠**（Collapsed）

> **可见性选项对照**：可视=显示；**已折叠=完全不显示不占位（选这个）**；隐藏=不显示但占位；非命中测试=显示但鼠标穿透。
> 蓝图对应：显示时 `Set Visibility(可视)`，隐藏时 `Set Visibility(已折叠)`。

### 2.4 NetPanel（网络面板）——"叠层"容器用法

> **容器选择口诀**：要层叠用 Overlay（底图+文字浮层），要排队用垂直/水平框（击杀播报），要自由摆用 Canvas，要固定大小套 Size Box。
> NetPanel 需要"半透明底图 + 文字浮在上层"→ 用 Overlay；**两个文本若直接放进 Overlay 会叠在同一点**，所以要用垂直框排队。

1. `覆盖层` → 改名 `NetPanel` → 锚点**铺满** → 可见性 **已折叠**
2. **底图（叠层第 1 层）**：拖 `图像` 进 NetPanel → 改名 `NetBackdrop`：
   - 外观 → 笔刷 → 颜色：R/G/B=0、**A=0.5**
   - 槽位对齐保持填充 → 自动全屏
3. **文字（叠层第 2 层）**：拖 `垂直框` 进 NetPanel → 改名 `TrafficBox`：
   - 选中它 → Details 顶部 **槽位区域**（显示 NetPanel 字样）：对齐 水平=填充 / 垂直=居中；**填充**：左 60、顶 100（离屏幕边缘留白）
   - 拖两个 `文本` **进 TrafficBox**（垂直框自动让它们竖排，不会重叠）：
     - `SentText`：内容 `↑ 0 KB/s`；字体 16
     - `RecvText`：内容 `↓ 0 KB/s`；字体 16
4. 确认 `NetPanel` 可见性 = **已折叠** → 编译 → 保存

> **想预览折叠的面板**：临时把可见性改回"可视"看效果，确认后改回"已折叠"再保存。

### 2.5 最终层级树

```
WBP_HUD
└── CanvasPanel_0
    ├── Crosshair        (文本 "+", 锚居中)
    ├── PlayerIdText     (锚左上 20,20)
    ├── RTTText          (锚左上 20,55)
    ├── HPBar            (进度条, 锚左下, 绿色)
    ├── HPText           (锚左下)
    ├── AmmoText         (锚右下)
    ├── WeaponText       (锚右下)
    ├── KillFeed         (垂直框, 锚右上)
    ├── DeathOverlay     (Overlay 铺满, 已折叠)
    │   ├── DeathBackdrop (图像, 黑 A=0.6, 填充)
    │   └── DeathText     (居中)
    └── NetPanel         (Overlay 铺满, 已折叠)
        ├── NetBackdrop   (图像, 黑 A=0.5, 填充)
        └── TrafficBox    (垂直框, 填充+留白)
            ├── SentText  (↑ 0 KB/s)
            └── RecvText  (↓ 0 KB/s)
```

**设计意图标注**：代码只引用 `DeathOverlay / NetPanel`（切可见性）和各 Set 函数目标控件——遮罩/面板的**子组件命名随意**（纯视觉，不被代码引用）；判断标准一条：**蓝图会引用的命名必须规范，纯展示的随意**。

### 2.6 新手高频坑

| 坑 | 现象 | 解法 |
|---|---|---|
| 控件拖到画布外 | 运行时看不见 | 选中看 Details 位置数值，手动改 |
| 全屏 Overlay 忘设折叠 | PIE 一进就黑屏/面板常驻 | 行为 → 可见性 → 已折叠 |
| 锚点全用默认左上 | 分辨率一变弹药跑到左上 | 每控件按 2.2 表格设对应角锚点 |
| Overlay 里两个文本叠一起 | 直接拖两个文本进 Overlay | 文本外面套垂直框排队 |
| 找不到画布/显示引擎内容 | 设计器空白无从下手 | 见 2.1 第 2 步；引擎内容开关是另一回事（Content Browser 齿轮） |

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
