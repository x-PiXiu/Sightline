# Sightline Client (UE5)

C++ 为主，蓝图只做表现层。

## Lyra 素材依赖（不入库）

以下素材来自 Epic 官方 **Lyra Starter Game**（Epic Games 启动器 → 示例 → Lyra Starter Game 获取）：
角色模型 / 动画 / 武器 / 音效。仓库不含 Lyra 内容文件——获取后按 `docs/episodes/` 中对应期目的说明导入。

## 第 3 期 Content 依赖（全部不入库，按蓝图操作方案重建）

- 自建资产统一放 `/Game/Code/`（Input / Character / UI / Game / Map）。仓库只入 C++ 源，以下 `.uasset` 均需按 `docs/episodes/ep03/蓝图操作方案.md` 手工重建：输入资产（IMC/IA）、`BP_Character_Manny/Quinn`、`ABP_Mannequin` 及 BS/AO 动画图、三个 WBP、`BP_GameMode_Sightline`（类默认值填 MannyClass/QuinnClass）。
- 对局地图 `NewMap`（`/Game/Code/Map/NewMap`）：自建图，需含 `Spawn_1`~`Spawn_4` 出生点（命名/Tag/Label 三者其一即可匹配）；曾试 Fab 商城 Palms 场景图，最终未采用。
- 双 PIE 联调前建议先降载（控制台 `sg.ResolutionQuality 50` 等），避免主线程卡顿触发 LAG 误判。
