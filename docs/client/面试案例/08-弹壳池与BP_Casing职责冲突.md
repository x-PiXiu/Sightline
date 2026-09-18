# 案例 08 · 弹壳池与 BP_Casing 的职责冲突——对象池的"复活序列"

> **标签**：对象池 · 组件级 Tick · 生命周期契约 · StopSimulating
> **一句话**：对象池假设"弹壳永不销毁、激活即满血复活"，而资产化的 BP_Casing 带着自己的
> 生命周期和运动组件——四组假设错配，四个真实 bug。

---

## 一、架构（重构后的最终分工）

```
C++ 池（ASightlineWeapon）——发令员
  生成 ×10 → 隐藏/冻结 → 入池待命
  开火：从池取出 → 注入参数（位置/初速/翻滚）→ 激活
  超限：回收最早的回池（永不销毁）

BP_Casing（蓝图）——演员
  ProjectileMovement：按注入初速 + 重力画抛物线
  RotatingMovement：随机三轴翻滚
  OnComponentHit：落地播 Shell_Impact 音 → 旋转归零（趴地静卧）
```

**契约**：C++ 管**生命周期与激活参数**；BP 组件管**飞行表现**（自驱）。

## 二、四组冲突（假设错配 → 真实 bug）

| # | BP_Casing 的行为 | C++ 池的假设 | 症状 |
|---|---|---|---|
| ① | **ProjectileMovement 是组件级 Tick**（不受 Actor 关 Tick 影响） | 入池"隐藏+关 Actor Tick"就安全了 | 开局 10 个隐形弹壳带初速乱飞，误触落地音效 |
| ② | 命中无弹跳 → **StopSimulating 解除运动目标**（UpdatedComponent 清空） | 激活 = SetActive(true) 即可复活 | 复用弹壳**悬空在抛壳口原地旋转**——有翻滚没飞行 |
| ③ | **BeginPlay 注入的初速只跑一次** | 复用弹壳会自动获得抛壳初速 | 复用弹壳零初速直坠 |
| ④ | **InitialLifeSpan 自毁**（BP 设计了自动消失） | 池假设"永不销毁" | ⚠️ **池枯竭**：寿命到期 10 个弹壳全灭，之后弹壳永久绝迹 |

## 三、修复（C++ 池侧的"复活序列"）

### 激活序列（顺序敏感！）

```cpp
// GetCasingFromPool 激活块
PM->SetUpdatedComponent(Casing->GetRootComponent());  // ① 重新绑定运动目标
PM->Velocity = EjectDir * rand + FVector(0,0,rand);   // ② 注入抛壳初速
PM->SetActive(true);                                   // ③ 激活
RM->RotationRate = 随机 ±1800;                         // ④ 重置翻滚速率
RM->SetActive(true);
// ⑤ 扫掠忽略武器与角色（出生点在枪身内部，防首帧碰撞清零初速）
```

**为什么 ① 必须在最前**：StopSimulating 清空了 UpdatedComponent——不重绑，
②的速度无处施加、③的激活无意义。**SetUpdatedComponent → Velocity → SetActive**
是 UE 抛射物对象池的标准复活序列。

### 入池序列（对称的"冻结"）

```cpp
隐藏 + 关 Actor Tick + 关物理模拟
+ ProjectileMovement.SetActive(false)   ← 防隐形乱飞/误触音效
+ RotatingMovement.SetActive(false)     ← 防隐形乱转
```

### 附加修复

| 修复 | 说明 |
|---|---|
| **池空兜底回收** | 原回收条件（在役数≥20）永远够不到（池只有 10）→ 改为"池空且有在役弹壳时强制回收最早的"——真正的环形复用 |
| **MoveIgnoreActors** | 出生点在枪身网格内部，首帧扫掠命中枪体 → 速度清零直坠。弹壳组件的 MoveIgnoreActors 加入武器与角色 |
| **CasingPoolSize 可配置** | EditDefaultsOnly 暴露——想地上更多弹壳调大即可 |

### BP 侧残留修改（需要手动）

**DoOnce 不重置** → 复用弹壳落地静音。改为**速度阈值门**（自复位）：
`命中事件 → GetVelocity → VectorLength > 100 → 播音效`——高速弹出/落地时响，
滚动减速后静音，无需任何重置。

## 四、追问演练

**Q：为什么 RotatingMovement 复用正常、ProjectileMovement 不正常？**
A：StopSimulating 是 ProjectileMovement 特有的停止逻辑（解除 UpdatedComponent）；
RotatingMovement 没有停止语义，永远旋转。所以症状是"翻滚正常、飞行消失"——
两个组件的差异恰好构成诊断证据。

**Q：为什么不用物理模拟（Simulate Physics + Impulse）做弹壳？**
A：可以，但池入池出需要反复开关物理模拟（休眠/唤醒开销、穿透风险），且物理弹壳
的碰撞查询成本更高。ProjectileMovement 是运动学方案：确定性弧线、扫掠可控、
组件停启干净——池化场景更合适。

**Q：MaxActiveCasings=20 与池大小 10 的错配是怎么发现的？**
A：现象倒推——"弹壳一直躺在地上"+"第 11 发起没弹壳"→ 池空返回 null →
检查回收条件：在役数(10) ≥ 上限(20) 永假 → 回收永不触发。**数值型 bug 用
边界分析比断点更快**。

## 五、30 秒速答卡

> "弹壳对象池和资产化 BP_Casing 结合时踩了四组冲突：组件级 Tick 不受 Actor 关 Tick
> 影响（开局隐形乱飞）、ProjectileMovement 落地停止会解除运动目标（复用弹壳悬空）、
> BeginPlay 初速只生效一次（复用直坠）、BP 自毁生命周期掏空池（弹壳绝迹）。
> 解法是把池的契约明确为'生命周期+激活参数'：入池冻结全部运动组件，激活时按
> SetUpdatedComponent→注入初速→SetActive 的标准序列复活，外加扫掠忽略武器角色。
> 本质是**对象池与资产化 Actor 的职责边界问题**——池管生死，资产管表演。"

## 六、涉及代码

- `ASightlineWeapon::InitCasingPool / GetCasingFromPool / FireShot`
- `BP_Casing`（ProjectileMovement + RotatingMovement + OnComponentHit）
- 修复提交：UE 仓库 `e779ef2 / 486bc7d / 363ec02 / a956426`
