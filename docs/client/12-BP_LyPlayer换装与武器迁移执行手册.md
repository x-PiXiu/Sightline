# 12 · BP_LyPlayer 换装与武器迁移执行手册

> **背景**：项目从另一台机器整体拷回，需要把"换了 Manny 骨骼但没动画、VIRTUS 武器不可见、
> Lyra 资产未接线"的半成品状态，推进到全链路可玩。
> **性质**：执行手册——每个模块含操作步骤、验证标准、回退说明，按依赖关系严格排序。
> **前置状态**：GameMode 已指向 BP_LyPlayer；Lyra 人物/武器资产已迁移；物理材质 5 个齐全；
> DA_SightlineImpactFX 存在；**DLL 过期（早于 P2 客户端代码）；DA_SightlineItemConfig 缺失**。
> **对应代码**：C++ 已全部就位（P2 道具 + 贴画系统），本手册只动蓝图/资产/配置。

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
| D | VIRTUS 不可见 / 动画不配套 | 45 分钟 |
| E | P2 道具 DA 缺失 | 15 分钟 |
| F | 换弹只有枪动没有手臂动 | 10 分钟 |
| G | 全链路验收 | 30 分钟 |
| H | 退役资产清场 | 验证全绿后 |

**铁律：A 没做完不开编辑器；H 在 G 全绿之前一律不动。**

---

## 模块 A：C++ 重编译（工程基线）

### 为什么必须最先做

`Binaries/Win64/UnrealEditor-FPS_Game.dll` 日期为 9月16 19:57，**早于 P2 道具系统的客户端代码**
（SightlineItemActor / DA 配置类 / Character 路由分支）。DLL 与源码不一致时打开编辑器，
引用这些类的资产（即将创建的 DA_SightlineItemConfig）会报"类不存在"或更糟的静默错乱。

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
| Default Weapon Class | `BP_Weapon_VIRTUS`（模块 D 换皮） |
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

## 模块 D：武器换皮 SK_Rifle

### D1. 最小武器 ABP（武器网格体的动画宿主）

内容浏览器 → 右键 `SK_Rifle_Skeleton` → Create → Animation Blueprint →
命名 `ABP_Weapon_Rifle_New` → 打开 → **Output Pose 什么都不接** → 编译保存。
（作用：给武器网格体一个 AnimInstance，蒙太奇才有地方播。）

### D2. 武器蓝图换皮

打开 `BP_Weapon_VIRTUS`：

| 属性 | 旧 | 新 |
|---|---|---|
| Default Skeletal Mesh | VIRTUS 模型（断链） | `SK_Rifle` |
| Default Anim Class | 旧武器 ABP | `ABP_Weapon_Rifle_New` |
| Reload Sound | 旧 | `Rifle_Load01`（Lyra 迁来的上膛音） |

其余（弹药/射速/冷却）不动。

### D3. 删除 Cast to BP_Player 链（武器解耦）

事件图表里 `Cast to BP_Player → Get ArmsMesh → Play Anim Montage` 整段**删除**——
武器不再碰角色网格体（职责契约：角色动画角色播）。空出的职责在模块 F 由角色侧接管。

### D4. 武器自身动画（枪身蒙太奇）

事件 `BP_OnReloadStarted`（武器自己的，已存在）追加：

```
Get Mesh（武器自己的 SkeletalMeshComponent）
  → Get Anim Instance → Montage Play（AM_Weap_Rifle_Reload）
```

效果：换弹时**枪身弹匣抽插动画**在武器上播，与角色无关。

### D5. 挂点与枪口（在 BP_LyPlayer 里）

1. Event `BP_OnWeaponEquipped` → 武器 Attach 目标 = `Get Mesh`（CharacterMesh0），
   插槽 = **`hand_r`**，Snap to Target；
2. 双击 `SK_Rifle` 检查骨架有无枪口插槽（骨架树搜 muzzle/socket）；
   有 → 武器蓝图 MuzzleOffset 对准；无 → 游戏里看枪口火焰位置，调 MuzzleOffset 至枪管前端。

### D 验证（PIE）

- [ ] 第一人称能看到手里的枪（贴合右手，不穿模不悬浮）
- [ ] 开火：枪口火焰位置正确、有枪声
- [ ] 按 R 换弹：枪身弹匣动画播放 + 上膛音效

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

## 模块 F：角色侧蒙太奇（手臂动画）

原"Cast 拿 ArmsMesh 播蒙太奇"的职责移交角色蓝图：

BP_LyPlayer 事件图表 → 添加 **Event BP_OnReloadStarted**（GameState 组件的事件）：

```
Get Mesh（CharacterMesh0）→ Play Anim Montage（AM_MM_Rifle_Reload）
```

### F 验证

- [ ] 按 R：**手臂抬枪（角色播）+ 弹匣抽插（武器播）同时发生**
- [ ] GameMode 换任意玩家蓝图，武器行为不变（解耦成功的标志）

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
| `ABP_Player` / `ABP_VIRTUS` | 旧手臂/旧武器骨架的 ABP |
| `AnimModifiers/` | 制作期工具，断链（强制删除安全，引用者是动画的编辑器历史） |
| `AnimNotifies/`（AN_ 开头） | GAS 通知，断链 |
| VIRTUS 残留引用（XL_FPSPack 武器网格/动画引用） | 已被 SK_Rifle 取代 |

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
