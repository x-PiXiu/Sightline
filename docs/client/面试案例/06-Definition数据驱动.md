# 案例 06 · Definition 数据驱动——从"抄四个事件"到"零事件节点"

> **标签**：抽象时机 · 开闭原则 · 数据驱动 · 三次法则
> **一句话**：每加一把枪要重抄四个蓝图事件——痛点触发抽象，推导出
> "C++ 行为 + Base 通用事件 + 每枪一个 DA"的三层终态，加新枪降到 30 分钟。

---

## 一、痛点（Situation）

按 12 手册为 Lyra 步枪新建 `BP_Weapon_Rifle` 时发现：四个蓝图事件
（OnShotImpact/OnAmmoChanged/OnReloadStarted/OnFireEffects）要从旧 VIRTUS
蓝图**逐个复制接线**。问题是——**下一把枪（Pistol 资产已迁）还要再抄一遍**。
复制粘贴式继承 = 每把枪重复同一份接线逻辑 = 修改一次要同步 N 处。

## 二、方案推导（Action · 先自己设计，再优化自己的设计）

### 第一版方案（直觉版）

"建一层抽象实现所有事件 + 枚举控制素材 + 中央 DA 管数据，
新武器继承抽象层 + 加枚举 + 中央 DA 加条目。"

### 对第一版的自我优化（三处）

**① 枚举可以整个省掉。** 枚举的作用是查表 key。但武器与道具不同：
道具需要 TypeId 因为服务器在网络下发它；**武器的身份就是"装备了哪个 Actor"**，
装备即指向，DA 实例本身就是数据载体——`BP_Weapon_Rifle.Definition = DA_Weapon_Rifle`
直接引用，无查表。省掉枚举 = 加新枪不改任何枚举定义、无"枚举与 DA 忘记同步"的 bug。

**② 中央 DA → 每武器一个 DA 实例。** 中央 `TMap<枚举,素材包>` 的问题：
所有武器挤一个文件（并行编辑冲突）、越滚越大。改为
`USightlineWeaponDefinition`（DataAsset 类）+ 每武器一个实例——
加新枪 = **复制一个 DA 填字段**，零代码零枚举零中央文件。

**③ 四个事件里两个是"绝对通用"的，不需要抽象。**

| 事件 | 读武器数据？ | 归宿 |
|---|---|---|
| `BP_OnShotImpact` | **完全不读**（查 DA_SightlineImpactFX 按表面类型，与武器无关） | Base 写死一次 |
| `BP_OnAmmoChanged` | **完全不读**（只转发数字给 HUD） | Base 写死一次 |
| `BP_OnReloadStarted` | 读（哪段枪身蒙太奇） | Base：`MontagePlay(Definition.GunReloadMontage)` |
| `BP_OnFireEffects` | 读 | Base：读 Definition，或留空 |

所以抽象层不是"实现四事件再按枚举分发"，而是**前两个写死、后两个读 Definition**——
比枚举方案还少一层间接。

### 第二版方案（终态三层）

```
ASightlineWeapon (C++ · 行为不变)
  + Definition : USightlineWeaponDefinition*   ← 唯一新增
  + BeginPlay 回填资产槽（Definition 非空字段优先）
  + FireShot/Reload 自动播 GunFire/GunReloadMontage
        │ 继承
┌───────▼──────────────────────────────┐
│ BP_Weapon_Base（这次有灵魂的 Base）     │
│  ShotImpact=贴画三件套（写死·通用）      │
│  AmmoChanged=HUD 刷新（写死·通用）      │
│  ReloadStarted=MontagePlay(Def.GunReload)│
└───────┬──────────────────────────────┘
        │ 继承（子类零事件节点）
   ┌────┴─────────────┐
BP_Weapon_Rifle   BP_Weapon_Pistol(未来)
Definition=       Definition=
 DA_Weapon_Rifle   DA_Weapon_Pistol
```

`USightlineWeaponDefinition`（DataAsset）字段：外观（Mesh/AnimClass/MuzzleOffset）、
音效（Fire/Reload）、枪身蒙太奇（GunFire/GunReload）、
**手臂蒙太奇（ArmFire/ArmReload）**、P8 预留（伤害/射速/弹匣）。

## 三、一个容易漏掉的设计点：手臂蒙太奇为什么也进 Definition

换弹时**角色侧**要播手臂动画（BP_LyPlayer 的 BP_OnReloadStarted 钩子），
它需要知道"当前武器的手臂换弹动画是哪段"。若这段数据放角色侧的表，
加新枪就要改角色——耦合回去了。正确做法：**数据收口在武器 Definition**，
C++ 提供 `GetArmReloadMontage()`（BlueprintPure），角色侧
`GetCurrentWeapon → GetArmReloadMontage → PlayAnimMontage`——
**加新枪角色侧也零改动**。判定标准：**数据跟着它的 Owner 走，而不是跟着使用者走**。

## 四、与 P8 的关系：不是抢跑

P8（数据驱动武器）的终态是 Definition 里加服务端契约字段（伤害/射速/弹匣）+
服务器数据表同步。现在做的是它的**地基**（Definition 结构 + Base 事件宿主），
方向完全一致、不是重复劳动。YAGNI 风险评估：唯一的赌注是"第二把枪会出现"，
而 Pistol 资产已迁好、路线图 P8 已立项——赌注基本必赢。

## 五、结果

- `BP_Weapon_Rifle` 事件图表**空的**（全继承）——数据驱动生效的标志；
- 加新枪步骤收敛为：复制 DA 填字段 → 建空蓝图指 Definition → 角色指新枪 ≈ 30 分钟，
  零事件节点、零 C++ 改动、角色侧零改动。

## 六、追问演练

**Q：为什么不一开始（第一把枪）就做 Base 抽象？**
A：第一把枪时 Base 没有内容可放（无共享逻辑），先建只会得到空壳（旧 BP_Weapon_Base
正是这样变成音效容器的——审计实锤它只有旧 VIRTUS 音效默认值，零事件实现）。
抽象的正确时机是**重复真实出现时**（第二次复制四个事件的痛感），即三次法则的
UE 蓝图版。过早抽象的成本我们已经付过一次学费。

**Q：Definition 为什么是"每枪一个 DA"而不是"一个 DA 装 N 把枪"？**
A：每枪一个实例：复制即新枪、无中央文件冲突、DA 即武器数据全集、
天然支持 P8 的服务端数据表一一对应。中央表的唯一优势是"一处看全部"，
编辑器里按类型过滤 DataAsset 也能达到。

**Q：C++ 直继 vs BP 基类，多武器到底选哪个？**
A：取决于 BP 基类层有没有**真实的共享逻辑**。审计发现旧 BP_Weapon_Base 只有
会被子类覆盖的音效默认值——这层无价值。行为共享已由 C++ 承担，所以直继 C++
更干净。若未来多个武器 BP 真的涌现出重复蓝图逻辑，规则是**下沉到 C++**
（规则归 C++），而不是建 BP 中间层。

**Q：BeginPlay 回填会不会覆盖蓝图里手动设的资产？**
A：方向是 Definition 优先、蓝图默认值兜底——只有 Definition 字段非空才覆盖。
两者都支持：纯 Definition 驱动（推荐）或无 Definition 的裸蓝图武器（向后兼容）。

## 七、30 秒速答卡

> "加第二把枪时要抄四个蓝图事件，我意识到抽象时机到了。第一版方案是枚举加
> 中央 DA，自我评审后改了三处：枚举省掉——武器身份就是装备的 Actor，DA 实例
> 直接引用；中央 DA 改成每枪一个实例——复制 DA 填字段就是新枪；四个事件审计后
> 发现两个与武器完全无关（贴画查表、HUD 转发），在基类写死一次，另外两个从
> Definition 读资产。终态是 C++ 行为 + Base 通用事件 + 每枪一个 DA 三层，
> 新武器蓝图事件图表是空的——加一把枪 30 分钟，零代码零事件节点，
> 连角色侧的手臂蒙太奇都从武器 Definition 读，角色零改动。"

## 八、涉及代码

- `SightlineWeaponDefinition.h`（DataAsset：9 字段）
- `SightlineWeapon.h/.cpp`（Definition 属性 + BeginPlay 回填 + 枪身蒙太奇自动播 +
  GetArm*Montage getters）
- 架构全文：[13-武器系统模块协作与数据流](../架构设计/13-武器系统模块协作与数据流.md)
