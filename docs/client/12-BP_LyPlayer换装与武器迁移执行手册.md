# 12 · BP_LyPlayer 换装与武器迁移执行手册

> **背景**：项目从另一台机器整体拷回，需要把"换了 Manny 骨骼但没动画、VIRTUS 武器不可见、
> Lyra 资产未接线"的半成品状态，推进到全链路可玩。
> **性质**：执行手册——每个模块含操作步骤、验证标准、回退说明，按依赖关系严格排序。
> **前置状态**：GameMode 已指向 BP_LyPlayer；Lyra 人物/武器资产已迁移；物理材质 5 个齐全；
> DA_SightlineImpactFX 存在；**DLL 过期（早于 P2 客户端代码）；DA_SightlineItemConfig 缺失**。
> **对应代码**：C++ 已全部就位（P2 道具 + 贴画系统 + USightlineWeaponDefinition），本手册只动蓝图/资产/配置。

### 执行进度追踪（随执行更新）

| 模块 | 状态 | 备注 |
|---|---|---|
| A C++ 重编译 | ✅ 已完成 | 含 `USightlineWeaponDefinition`（2026-09-19 编译通过） |
| B 角色成立 | ✅ 主体完成 | BP_LyPlayer 已建、GameMode 已指向；B5 的 Default Weapon Class 待 D4 后切换 |
| C HUD 解耦 | ⚠️ 待核对 | WBP_HUD 等的 Cast 目标逐个确认 |
| D 武器新建 | ❌ 未开始 | ABP_Weapon_Rifle / DA_Weapon_Rifle / BP_Weapon_Rifle 均未创建 |
| E 道具补完 | ❌ 未开始 | `DA_SightlineItemConfig` 确认仍缺失 |
| F 蒙太奇 | ❌ 未开始 | 已并入 D5 第 3 步 |
| G 双开联调 | ❌ | 依赖 D/E |
| H 清场 | ❌ | G 全绿后 |

---

## 总览：七个模块的依赖关系

```
A. C++ 重编译 ──▶ B. BP_LyPlayer 角色成立 ──▶ D. 武器换皮 SK_Rifle ──▶ F. 角色侧蒙太奇
                        │                        │
                        ├──▶ C. HUD/蓝图解耦     └──▶ E. 道具系统补完
                        │
                        └──▶ G. 双开全链路联调 ──▶ H. 退役清场
```

| 模块 | 解决什么 | 预计耗时 |
|---|---|---|
| A | DLL 与源码不一致（P2 道具 C++ 没编进去） | 5 分钟 |
| B | 角色无动画 / 组件缺失 / 相机 | 30 分钟 |
| C | HUD 铸造旧玩家类 → 换新 Pawn 后运行时失效 | 15 分钟 |
| D | VIRTUS 不可见 / 动画不配套——**新建 BP_Weapon_Rifle**（逻辑从旧蓝图复制） | 60 分钟 |
| E | P2 道具 DA 缺失 | 15 分钟 |
| F | 换弹只有枪动没有手臂动 | 10 分钟 |
| G | 全链路验收 | 30 分钟 |
| H | 退役资产清场 | 验证全绿后 |

**铁律：A 没做完不开编辑器；H 在 G 全绿之前一律不动。**

---

## 模块 A：C++ 重编译（工程基线）

### 为什么必须最先做

DLL 此前落后于源码两代：先是 P2 道具系统（SightlineItemActor / Character 路由分支），
现在是**武器数据驱动**（`USightlineWeaponDefinition` / `ASightlineWeapon.Definition`，
2026-09-19 已编译通过）。DLL 与源码不一致时打开编辑器，引用这些类的资产
（模块 D/E 即将创建的 DA_Weapon_Rifle、DA_SightlineItemConfig）会报"类不存在"
或更糟的静默错乱。**每次 C++ 合入后、开编辑器前，都重跑本模块。**

### 操作

关闭编辑器（如开着），命令行执行（Git Bash）：

```bash
"/d/GameEngine/Epic/EpicEngine/UE_5.7/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" \
  FPS_GameEditor Win64 Development \
  -project="D:/workspace/Demo/UnrealDemo/FPS_Game_OnlineSelf/FPS_Game.uproject" -WaitMutex
```

### 验证

- 输出 `Result: Succeeded`；
- DLL 日期更新为当前时间。

---

## 模块 B：BP_LyPlayer 角色成立

GameMode 已指向 BP_LyPlayer，Manny 已显示但没动画。按清单逐项核对（打勾式推进）：

### B1. CharacterMesh0（网格体）

| 属性 | 值 | 验证 |
|---|---|---|
| 骨骼网格体资产 | `SKM_Manny`（Content/Characters/... Lyra 版） | 视口可见模型 |
| 动画类 | `ABP_Manny`（**引擎自带** Engine Content，过渡用） | 视口里模型摆出 Idle 姿势 |
| 位置 | `X:0, Y:0, Z:-90` | 脚底贴胶囊底 |
| 旋转 | `Yaw = -90`（进游戏横着走改 +90） | 箭头朝向与模型面朝一致 |
| First Person Primitive Type | `First Person`（有此属性就设） | — |

### B2. 相机

1. 无相机组件则**添加** → Camera Component，挂在**胶囊体下**（勿挂网格下）；
2. 位置 `X:0, Y:0, Z:75`（挂 head 插槽可选——位置跟动画晃、旋转跟鼠标的方案）；
3. **Use Pawn Control Rotation = ✅**（不勾鼠标不转视角）；
4. FOV = 90。

### B3. 三个 Sightline 组件（缺一游戏跑不起来）

| 组件 | 找不到的症状（PIE 日志） |
|---|---|
| SightlineGameStateComponent | `[Char] 找不到 GameStateComponent` |
| SightlineSessionComponent | `[Char] 找不到 SessionComponent` |
| SightlineRemotePawnManagerComponent | `[Char] 找不到 RemotePawnManager` |

任一缺失：**添加** → 搜索对应类名 → 添加并命名（建议与旧 BP_Player 同名，方便事件图表引用恢复）。

### B4. 组件属性

- SightlineRemotePawnManager → **Remote Pawn Class** = `BP_RemotePawn`（新组件默认空，必填）。

### B5. Class Defaults（对照旧 BP_Player 抄）

| 属性 | 值 |
|---|---|
| Player Name | 旧值（如 UEPlayer） |
| Default Weapon Class | 暂维持 `BP_Weapon_VIRTUS`，**模块 D4 建成后**再切 `BP_Weapon_Rifle`（D4 之前它还不存在） |
| Auto Login | ✅ |
| Conn Config | 旧值（默认 127.0.0.1:8888） |
| Move Report Interval / Ping Interval | 默认 0.1 / 2 |

### B6. 事件图表

从旧 BP_Player 复制的图表：编译后检查有没有红色/警告节点（引用已删组件的），
逐个修复——组件用**同名**添加的话引用会自动恢复。

### B 验证（PIE）

- [ ] 视角在眼睛高度，鼠标四向转视角正常
- [ ] 走路/跑步腿部有动画（ABP_Manny 生效）
- [ ] 日志无 `找不到 xxxComponent` Error，登录进房正常（`[State] PlayerId=xx`）
- [ ] HUD 显示正常（若 HUD 空白 → 模块 C）

### B 回退

改坏就删 BP_LyPlayer 重建（内容浏览器右键旧 BP_Player 可复制一份当模板）——
**但不要动 BP_Player 本体**，模块 C 完成前它还是 HUD 的 Cast 目标。

---

## 模块 C：HUD/蓝图解耦——Cast 目标改为 C++ 基类

### 为什么必须做

GameMode 的 Pawn 已换成 BP_LyPlayer。所有 `Cast to BP_Player` 的蓝图
（WBP_HUD、击杀提示等）**编译能过但运行时 Cast 必失败**（场景里的 Pawn 是 BP_LyPlayer）
→ HUD 全空白。改 Cast 目标为 C++ 基类后，与具体玩家蓝图的生死彻底解耦。

前置事实：WBP_HUD 用到的函数（GetPlayerId / GetHP / GetGameState / IsDead）
**全部定义在 C++ 基类 `ASightlineCharacter` 上**，蓝图子类零新增——替换零功能损失。

### 操作（每个受影响蓝图重复）

1. 找全名单：内容浏览器右键 `BP_Player` → **引用查看器** → 左侧列出所有引用者；
2. 打开其中一个（如 WBP_HUD）→ 编译 → 双击错误跳到 `Cast to BP_Player` 节点；
3. 删除该节点 → 添加 **Cast to SightlineCharacter** → 按原样接回（函数节点不用动，函数名相同）；
4. 编译 → 绿 → 下一个。

### 验证

- [ ] 引用查看器里所有引用者逐一改完、逐个编译绿
- [ ] PIE：HUD 血条/弹药/延迟正常显示（此时 Pawn 是 BP_LyPlayer）

---

## 模块 D：新建 BP_Weapon_Rifle（Definition 数据驱动终态）

> **架构升级（2026-09-19）**：本模块按"一步到位"方案执行——C++ 已新增
> `USightlineWeaponDefinition`（数据资产类）与 `ASightlineWeapon.Definition` 属性，
> 枪身蒙太奇由 C++ 自动播放，蓝图事件需求被压到只剩 2 个（贴画+HUD），
> 且都实现在 BP_Weapon_Base 里写一次、子类永久继承。
> 继承审计结论：旧 BP_Weapon_Base 只是音效默认值容器（无事件逻辑），已原地翻新为通用事件宿主；
> BP_OnAmmoChanged 在旧蓝图两级均未实现——HUD 弹药刷新实为 WBP_HUD 轮询式函数绑定。
> 数据流与协作时序详见 [`架构设计/13-武器系统模块协作与数据流.md`](架构设计/13-武器系统模块协作与数据流.md)。
>
> **资产放置规范**：所有新建文件进 `Content/Code/` 对应子目录，
> **Lyra 迁移目录（Content/Weapons、Content/Characters）只存资产本体，不新建文件**。
> **同步策略**：6 个 Lyra 目录（Audio/Characters/Effects/GameplayEffects/
> PhysicsMaterials/Weapons）已退出 git 跟踪（gitignore + rm --cached），
> 磁盘文件保留，机器间靠**整项目拷贝**同步——换机器后记得连同这些目录一起拷。

| 新文件 | 位置 |
|---|---|
| `ABP_Weapon_Rifle`（武器动画蓝图） | `Content/Code/Player/Animation/Weapon/` |
| `DA_Weapon_Rifle`（Definition 实例） | `Content/Code/DataAssets/` |
| `BP_Weapon_Base`（翻新：清旧默认值 + 写两个通用事件） | `Content/Code/Weapon/Base/`（原地） |
| `BP_Weapon_Rifle`（武器蓝图，父类 = BP_Weapon_Base） | `Content/Code/Weapon/` |

### D1. 建武器动画蓝图 ABP_Weapon_Rifle

1. 内容浏览器进入 `Content/Weapons/Rifle/Mesh/` → 右键 `SK_Rifle_Skeleton` →
   **创建 → 动画蓝图** → 命名 `ABP_Weapon_Rifle`；
2. ⚠️ 创建后默认落在 Lyra 目录——**拖到 `Content/Code/Player/Animation/Weapon/`**
   （编辑器内移动自动更新引用；禁止文件资源管理器剪切 .uasset）；
3. 打开 ABP → **AnimGraph** → ⚠️ **关键坑**：右键空白处 → 搜 `Slot` →
   添加 **"Slot 'DefaultSlot'"** 节点 → **Slot 输出连到 Output Pose** → 编译保存。

> 没有 Slot 节点 = `Montage Play` 播放的姿势没有出口 = 枪身动画"播了个寂寞"。

### D2. 建 DA_Weapon_Rifle（数据实例）

1. `Content/Code/DataAssets/` → 右键 → 杂项 → Data Asset → 选 **SightlineWeaponDefinition** →
   命名 `DA_Weapon_Rifle`；
2. 填字段：

| 分组 | 字段 | 值 |
|---|---|---|
| 外观 | Mesh | `SK_Rifle` |
| 外观 | Anim Class | `ABP_Weapon_Rifle`（D1 建的） |
| 外观 | Muzzle Offset | 先 0，D6 调 |
| 音效 | Fire Sound | 旧枪声可用则沿用 |
| 音效 | Reload Sound | `Rifle_Load01` |
| 蒙太奇 | Gun Reload Montage | `AM_Weap_Rifle_Reload`（枪身弹匣抽插，C++ 自动播） |
| 蒙太奇 | Gun Fire Montage | 可空（枪机动画） |
| 蒙太奇 | Arm Reload Montage | `AM_MM_Rifle_Reload`（手臂换弹，角色侧读） |
| 蒙太奇 | Arm Fire Montage | 可空 |

### D3. 翻新 BP_Weapon_Base（清壳 + 写两个通用事件）

打开 `Content/Code/Weapon/Base/BP_Weapon_Base`：

1. **清掉旧音效默认值**（FireSound/ReloadSound 指向 VIRTUS 音的清空——子类 Definition 会给）；
2. 从 `BP_Weapon_VIRTUS` **复制** `Event BP_OnShotImpact` 的贴画三件套
   （GetSubsystem + TryGetImpactFX + Spawn Decal / Play Sound / Spawn System）粘贴进来；
3. `Event BP_OnAmmoChanged`：可选接线（HUD 现为轮询绑定仍工作；迁推送时在此实现）；
4. 编译保存。

> Base 从此有灵魂：贴画/HUD 两个通用事件写一次，所有武器子类永久继承。

### D4. 建 BP_Weapon_Rifle（零事件节点）

1. `Content/Code/Weapon/` → 右键 → 蓝图类 → 父类选 **BP_Weapon_Base** → 命名 `BP_Weapon_Rifle`；
2. Class Defaults → **Definition** = `DA_Weapon_Rifle`（核心必填项——网格/动画/音效/蒙太奇全部由它提供）；
3. **Definition 之外的武器槽也要配**（这些不在 Definition 里）：
   - **Muzzle Flash Template** = 枪口火焰 Niagara（可沿用 `VIRTUS_MuzzleFlash`，或 Lyra 枪口特效）；
   - **Casing Class** = `BP_Casing`（弹壳池，不配则无弹壳）；
   - **Impact FX Config** = `DA_SightlineImpactFX`（贴画查表 + 预热，Base 的 BP_OnShotImpact 靠它查表）；
4. 编译保存。四个事件全部继承 Base，**一个节点都不用加**——这就是数据驱动生效的标志。

### D5. 接入角色（在 BP_LyPlayer 里）

1. Class Defaults → **Default Weapon Class** = `BP_Weapon_Rifle`；
2. Event `BP_OnWeaponEquipped` → 武器 Attach = `Get Mesh`（CharacterMesh0）、
   插槽 **`hand_r`**、Snap to Target；
3. Event `BP_OnReloadStarted`（GameState 钩子）→ 手臂蒙太奇：
   `GetCurrentWeapon → GetArmReloadMontage → CharacterMesh0.PlayAnimMontage`（可空判断）。

### D6. 进游戏微调

1. 枪贴合右手：D5 Attach 节点接 Transform 引脚微调；
2. 枪口火焰位置：DA 里的 Muzzle Offset 调至枪管前端。

### D 验证（PIE）

- [ ] 第一人称看得到枪、贴合右手
- [ ] 开火：枪声 + 枪口火焰正确 + 枪机动画（若配 GunFireMontage）
- [ ] 按 R：**枪身弹匣动画（C++ 自动播）+ 手臂换弹动画（角色读 Definition）同时发生**
- [ ] HUD 弹药正常增减
- [ ] 打墙弹孔/音效/碎屑正常
- [ ] BP_Weapon_Rifle 事件图表应为**空的**（全继承）——数据驱动生效的标志

---

## 模块 E：道具系统补完（P2 最后一公里）

### E1. 创建 DA_SightlineItemConfig

`Content/Code/DataAssets/` → 右键 → 杂项 → Data Asset → 选 **SightlineItemConfig** →
命名 `DA_SightlineItemConfig`。Entries 加两行：

| Key | Mesh | AmmoAmount | 说明 |
|---|---|---|---|
| `1`（血包） | `SM_healthpackFull`（Content/Weapons/Healthpack/Mesh/） | — | SpawnZOffset 先 0，半埋再调 |
| `2`（弹药箱） | `SM_grenade`（过渡用）或 Fab 的 ammo box | `60` | |

### E2. 接线

`BP_LyPlayer` → Class Defaults → **Item Config** = `DA_SightlineItemConfig`。

### E3. 贴画系统接线确认（另一台机器的成果，过一遍）

- 武器 Class Defaults → **Impact FX Config** = `DA_SightlineImpactFX`（在）；
- 武器事件图表 → `Event BP_OnShotImpact` 已接 Spawn Decal / Play Sound / Spawn System 三件套；
- 可选：`BP_OnItemSpawned / BP_OnItemTaken` 钩子接线（拾取特效音效）。

### E 验证（PIE，服务器须为 P2 版）

- [ ] 开枪打墙：弹孔 + 音效 + 碎屑出现（打不同材质表现不同）
- [ ] 地图出现道具模型（血包）
- [ ] 走上血包：拾取消失 + HP 回复（`[State] 治疗` 日志）+ 15 秒重生

---

## 模块 F：角色侧蒙太奇（已并入模块 D5 第 3 步）

> 原内容（BP_LyPlayer 的 `BP_OnReloadStarted` → `GetCurrentWeapon → GetArmReloadMontage`
> → `CharacterMesh0.PlayAnimMontage`）已作为 **D5 第 3 步** 执行，本模块仅保留编号
> 以维持引用稳定。验证标准不变：按 R 时**手臂换弹（角色播）+ 枪身弹匣（C++ 自动播）同时发生**。

---

## 模块 G：双开全链路联调

**前置**：服务器（192.168.1.35）运行 P2 版——`git pull` + 重新编译重启
（道具消息 ItemSpawn/ItemTaken 只有新服务器会发）。

### 验证清单

- [ ] 双方互见 Manny 傀儡、位置同步
- [ ] 命中/击杀/重生链路正常
- [ ] 打墙出弹孔，不同材质不同表现
- [ ] 血包：出现 → 拾取消失 → HP 回复 → 15 秒重生
- [ ] 弹药箱：备弹 +60
- [ ] 换弹：手臂 + 枪身动画同步
- [ ] RTT 显示正常（无贴地尖刺）
- [ ] 首枪无明显卡顿（预热日志：`[Weapon] 枪口火焰已绑定` → `[DecalPool] 预热` 序列正确）

---

## 模块 H：退役清场（G 全绿后才执行）

| 删除 | 原因 |
|---|---|
| `BP_Player` + `BP_Player_broken` + 备份文件 | 已被 BP_LyPlayer 取代 |
| `ABP_Player` / `ABP_VIRTUS` / `BS_VIRTUS` / `BS_FPSPlayer` | 旧手臂/旧武器骨架的 ABP 与混合空间 |
| **`BP_Weapon_VIRTUS`** | 已被 BP_Weapon_Rifle 取代（G 全绿确认后删） |
| `MS_VIRTUS`（Code/Weapon/MetaSound/） | 旧武器音效，无引用即删 |
| `AnimModifiers/` | 制作期工具，断链（强制删除安全，引用者是动画的编辑器历史） |
| `AnimNotifies/`（AN_ 开头） | GAS 通知，断链 |
| VIRTUS 残留引用（XL_FPSPack 武器网格/动画引用） | 已被 SK_Rifle 取代 |
| `Content/Characters/Heroes/Abilities/AN_Reload` | Lyra GAS 通知，断链（随 804 提交入库） |
| `Content/Characters/Heroes/PhysMat_Player` + `_WeakSpot` | Lyra 部位伤害物理材质，本项目用不上 |
| `Content/GameplayEffects/`（GE_Damage_Basic_Instant 等） | GAS 效果资产，断链 |
| `Content/Effects/AnimationNotifies/AN_FootPlant_*` | 先开文件验证是否断链再决定（可能是引擎通知可用） |

删除顺序：先解除引用（模块 C/D 已做）→ 普通删除（被拦就查引用者）→ Save All →
git commit Content 资产。

---

## 已知风险与回退

| 风险 | 缓解 |
|---|---|
| B 模块改坏 BP_LyPlayer | 删掉重建（复制 BP_Player 当模板），20 分钟损失 |
| D 换皮后枪位置怪 | hand_r 插槽 + Attach 的 Transform 引脚微调；或直接用 SM_Rifle 观察 |
| E 道具不出现 | 三查：服务器版本 → Item Config 槽 → DA 的 Key 是否 1/2 |
| G 联调 HUD 异常 | 检查模块 C 是否漏改某个蓝图（引用查看器核对名单） |
| 任何一步崩 | 停在原地截图发日志，勿连锁删除资产 |

---

*维护记录*

| 日期 | 修订 |
|---|---|
| 2026-09-19 | 初版：跨机器拷贝后的换装迁移执行手册——七模块依赖图、逐模块点击级步骤、验证清单、回退路径 |
| 2026-09-19 | 增加执行进度追踪表（A/B 完成，D/E 未开始）；模块 A 理由补 Definition 合入；B5 标注 DefaultWeaponClass 切换时机（D4 后）；D4 补 Definition 外武器槽（枪口火焰/弹壳/贴画配置）；模块 F 并入 D5；模块 H 补 804 提交带入的 Lyra 断链残留（AN_Reload/PhysMat_Player/GameplayEffects） |
| 2026-09-19 | 模块 D 升级为终态：C++ 新增 `USightlineWeaponDefinition` + `ASightlineWeapon.Definition`（BeginPlay 回填资产槽/枪身蒙太奇自动播/GetArm*Montage），蓝图事件需求压到 2 个并入 BP_Weapon_Base 写一次；模块 D 改为 D1 ABP → D2 DA → D3 翻新 Base → D4 零节点子类 → D5 角色接入 |
| 2026-09-19 | 模块 D 重写：原地改 BP_Weapon_VIRTUS → **新建 BP_Weapon_Rifle**（逻辑从旧蓝图按"抄/不抄"清单复制，贴画接线不丢）；新增资产放置规范（新文件一律进 Code/ 对应子目录，Lyra 目录不新建文件）；模块 H 补 BP_Weapon_VIRTUS / BS_VIRTUS / MS_VIRTUS 退役 |
