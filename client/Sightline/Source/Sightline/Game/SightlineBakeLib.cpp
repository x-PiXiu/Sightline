#include "Game/SightlineBakeLib.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Game/SightlineOccluderComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/TargetPoint.h"
#include "Character/SightlineCharacter.h"
#include "GameFramework/Character.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace {
    // 烘焙全局控制台命令（2026-10-29 用户实勘：Output Log 命令行走引擎级路由，到不了 Pawn 的
    // UFUNCTION(Exec)——命令被静默吞掉零输出）。注册为 IConsoleManager 命令后：PIE 视口控制台/
    // Output Log 命令行/编辑器 Cmd 全可达；PIE 在跑取 PIE 世界，否则取编辑器世界。
    // 结果同时打日志与屏幕（AddOnScreenDebugMessage——防日志洪峰刷过看不见）。
    UWorld* FindBakeTargetWorld() {
        if (!GEngine) return nullptr;
        UWorld* EditorWorld = nullptr;
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts()) {
            UWorld* W = Ctx.World();
            if (!W) continue;
            if (W->WorldType == EWorldType::PIE) return W;      // PIE 优先（验的是玩法场景）
            if (W->WorldType == EWorldType::Editor && !EditorWorld) EditorWorld = W;
        }
        return EditorWorld;
    }
}

FAutoConsoleCommand GBakeOccludersCmd(
    TEXT("SrvBakeOccluders"),
    TEXT("烘焙服务器几何合同（SightlineOccluder 标记 → server/assets/maps/<关卡>.json）"),
    FConsoleCommandDelegate::CreateLambda([]() {
        UWorld* Target = FindBakeTargetWorld();
        if (!Target) {
            UE_LOG(LogTemp, Warning, TEXT("[烘焙] FAIL 未找到可烘焙的世界（先打开关卡 NewMap）"));
            return;
        }
        FString Summary;
        const bool bOk = USightlineBakeLib::BakeServerOccluders(Target, FString(), Summary);
        UE_LOG(LogTemp, Log, TEXT("[烘焙] %s %s"), bOk ? TEXT("OK") : TEXT("FAIL"), *Summary);
        if (GEngine) GEngine->AddOnScreenDebugMessage(77001, 8.f, bOk ? FColor::Green : FColor::Red,
            FString::Printf(TEXT("[烘焙] %s %s"), bOk ? TEXT("OK") : TEXT("FAIL"), *Summary));
    }));

bool USightlineBakeLib::BakeServerOccluders(UWorld* World, const FString& OutPath, FString& OutSummary) {
    if (!World) {
        OutSummary = TEXT("World 为空");
        return false;
    }

    // 1) 扫描标记件 → 世界系盒（中心/半尺寸/yaw）。自定义覆盖：组件变换进合同（支持 yaw）；
    //    默认：Owner 的组件联合包围盒（世界 AABB——轴对齐，yaw=0）
    //    诊断双计数（2026-10-29 用户实勘"烘不进去"）：标记数=组件实例数；盒数=入合同数；
    //    标记>盒=实例包围盒无效被跳过（空 Actor 只挂标记件没有 Mesh 的典型症状）
    TArray<FVector> Centers;
    TArray<FVector> Halves;
    TArray<float>   Yaws;
    TArray<FString> MarkerNames;                         // 盒→Actor 名（净空告警点名——用户关卡里"不认识的箱子"靠它定位）
    int32 MarkersFound = 0;
    int32 Skipped = 0;
    for (TActorIterator<AActor> It(World); It; ++It) {
        AActor* Actor = *It;
        USightlineOccluderComponent* Marker = Actor->FindComponentByClass<USightlineOccluderComponent>();
        if (!Marker) continue;
        ++MarkersFound;
        FVector Center;
        FVector Half;
        float YawDeg = 0.f;
        if (Marker->bOverrideBounds) {
            const FTransform Xf = Marker->GetComponentTransform();
            Center = Xf.TransformPosition(Marker->CustomCenter);
            // 半尺寸随 yaw 旋转展开（合同原语="yaw 旋转的轴对齐盒"——pitch/roll 不支持，D5-08v2 边界）
            const FRotator Rot = Xf.Rotator();
            const float Rad = FMath::DegreesToRadians(Rot.Yaw);
            const FVector E = Marker->CustomHalfExtents;
            Half = FVector(
                FMath::Abs(E.X * FMath::Cos(Rad)) + FMath::Abs(E.Y * FMath::Sin(Rad)),
                FMath::Abs(E.X * FMath::Sin(Rad)) + FMath::Abs(E.Y * FMath::Cos(Rad)),
                E.Z);
            YawDeg = Rot.Yaw;
        } else {
            const FBox Bounds = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
            if (!Bounds.IsValid ||
                Bounds.GetExtent().X <= 0.f || Bounds.GetExtent().Y <= 0.f || Bounds.GetExtent().Z <= 0.f) {
                ++Skipped;                               // 空 Actor 只挂标记件无 Mesh=包围盒无效（此前静默=用户视角"烘不进去"无解释）
                continue;
            }
            Center = Bounds.GetCenter();
            Half = Bounds.GetExtent();
        }
        Centers.Add(Center);
        Halves.Add(Half);
        Yaws.Add(YawDeg);
        MarkerNames.Add(Actor->GetName());
    }
    if (MarkersFound == 0) {
        OutSummary = TEXT("目标世界里没有标记件实例——BP 资产加了组件≠关卡有实例（拖进当前关卡）；PIE 会话里新摆的墙要先重进 PIE");
        return false;
    }

    // 2.5) ep07：出生点（PlayerStart 命名合同 Spawn_{n}·三重匹配同 FindSpawnStart）
    //      与人机巡逻点（ATargetPoint·Tag=BotPatrol 才计入——不误收无关标记点）
    struct BakedSpawn { int32 Index; FVector Pos; float YawDeg; };
    TArray<BakedSpawn> Spawns;
    for (TActorIterator<APlayerStart> It(World); It; ++It) {
        auto ParseIndex = [](const FString& Str) -> int32 {
            if (!Str.StartsWith(TEXT("Spawn_"))) return INDEX_NONE;
            const int32 N = FCString::Atoi(*Str.Mid(6));     // Spawn_1 → 1 → 下标 0
            return N > 0 ? N - 1 : INDEX_NONE;
        };
        int32 Idx = ParseIndex(It->GetName());
        if (Idx == INDEX_NONE) Idx = ParseIndex(It->PlayerStartTag.ToString());
        if (Idx == INDEX_NONE) Idx = ParseIndex(It->GetActorLabel());
        if (Idx == INDEX_NONE) continue;                 // 非合同命名：不入合同（防误收引擎默认位）
        Spawns.Add({Idx, It->GetActorLocation(), float(It->GetActorRotation().Yaw)});
    }
    Spawns.Sort([](const BakedSpawn& A, const BakedSpawn& B) { return A.Index < B.Index; });

    TArray<FVector> Patrols;
    for (TActorIterator<ATargetPoint> It(World); It; ++It) {
        if (!It->ActorHasTag(TEXT("BotPatrol"))) continue;
        Patrols.Add(It->GetActorLocation());
    }

    // 2) entityHitbox = 角色胶囊真身（CDO 读取——烘焙期拿默认值，服务器不猜）
    float HitRadius = 40.f, HitHalfH = 90.f;
    if (const ASightlineCharacter* CharCDO = GetDefault<ASightlineCharacter>()) {
        if (const UCapsuleComponent* Capsule = CharCDO->GetCapsuleComponent()) {
            HitRadius = Capsule->GetUnscaledCapsuleRadius();
            HitHalfH = Capsule->GetUnscaledCapsuleHalfHeight();
        }
    }

    // 3) 产出几何合同 v1（手拼 JSON——客户端只写不读，解析在服务器 MapGeometryLoader）
    FString LevelName = World->GetName();
    // PIE 世界名=UEDPIE_{N}_{关卡名}——剥前缀（否则落盘 UEDPIE_0_NewMap.json，服务器找 NewMap.json 落空）
    if (LevelName.StartsWith(TEXT("UEDPIE_"))) {
        const int32 Sep = LevelName.Find(TEXT("_"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 7);
        if (Sep != INDEX_NONE) LevelName = LevelName.Mid(Sep + 1);
    }
    FString Json = TEXT("{\n");
    Json += TEXT("  \"map\": \"") + LevelName + TEXT("\",\n  \"unit\": \"cm\",\n");
    Json += FString::Printf(TEXT("  \"occluders\": [\n"));
    for (int32 i = 0; i < Centers.Num(); ++i) {
        Json += FString::Printf(TEXT("    { \"primitives\": [ { \"type\": \"box\", \"pos\": [%.2f, %.2f, %.2f], \"size\": [%.2f, %.2f, %.2f], \"yawDeg\": %.2f } ] }%s\n"),
                                Centers[i].X, Centers[i].Y, Centers[i].Z,
                                Halves[i].X, Halves[i].Y, Halves[i].Z, Yaws[i],
                                i + 1 < Centers.Num() ? TEXT(",") : TEXT(""));
    }
    Json += TEXT("  ],\n");
    // ep07：spawnPoints（序号升序——服务器 spawnPoints[i] ↔ 客户端 Spawn_{i+1} 同源合同）
    Json += TEXT("  \"spawnPoints\": [\n");
    for (int32 i = 0; i < Spawns.Num(); ++i) {
        Json += FString::Printf(TEXT("    { \"pos\": [%.2f, %.2f, %.2f], \"yawDeg\": %.2f }%s\n"),
                                Spawns[i].Pos.X, Spawns[i].Pos.Y, Spawns[i].Pos.Z, Spawns[i].YawDeg,
                                i + 1 < Spawns.Num() ? TEXT(",") : TEXT(""));
    }
    Json += TEXT("  ],\n");
    Json += TEXT("  \"patrolPoints\": [\n");
    for (int32 i = 0; i < Patrols.Num(); ++i) {
        Json += FString::Printf(TEXT("    { \"pos\": [%.2f, %.2f, %.2f] }%s\n"),
                                Patrols[i].X, Patrols[i].Y, Patrols[i].Z,
                                i + 1 < Patrols.Num() ? TEXT(",") : TEXT(""));
    }
    Json += TEXT("  ],\n");
    Json += FString::Printf(TEXT("  \"entityHitbox\": { \"radius\": %.2f, \"halfHeight\": %.2f }\n"), HitRadius, HitHalfH);
    Json += TEXT("}\n");

    // 4) 落盘（工程目录相对两仓同盘；目录不存在则创建）
    FString Path = OutPath;
    if (Path.IsEmpty()) {
        const FString LevelFile = LevelName + TEXT(".json");
        Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../server/assets/maps/"), LevelFile);
        FPaths::NormalizeFilename(Path);
    }
    const FString Dir = FPaths::GetPath(Path);
    if (!IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true)) {
        OutSummary = TEXT("目录创建失败：") + Dir;
        return false;
    }
    if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) {
        OutSummary = TEXT("写盘失败：") + Path;
        return false;
    }
    // 5) 出生点净空自检（ep07：箱体+命中体膨胀带 34cm 压住出生格 → 人机原地不动、子弹先中箱皮
    //    打不中它；与服务器 nav_grid.h 同一套语义：身体高度带过滤 + 34cm 膨胀。摆错烘焙当场红字，
    //    不用等服务器启动告警）。客户端不 include 服务器头——判定式按 kNav* 常量镜像手写。
    FString SpawnBlocked;
    {
        constexpr float Inflate = 34.f;                  // kNavInflateCm 镜像
        constexpr float StepCm = 45.f, BodyTopCm = 176.f; // kNavStepCm / kNavBodyTopCm 镜像
        for (const BakedSpawn& S : Spawns) {
            for (int32 i = 0; i < Centers.Num(); ++i) {
                const float Bot = Centers[i].Z - Halves[i].Z, Top = Centers[i].Z + Halves[i].Z;
                if (!(Top > StepCm) || !(Bot < BodyTopCm)) continue;   // 身体高度带外（地板等）：不挡
                const float Ex = Halves[i].X + Inflate, Ey = Halves[i].Y + Inflate;
                if (FMath::Abs(S.Pos.X - Centers[i].X) <= Ex && FMath::Abs(S.Pos.Y - Centers[i].Y) <= Ey) {
                    SpawnBlocked += FString::Printf(TEXT(" [Spawn_%d@(%d,%d) 压在盒#%d=%s]"),
                                                    S.Index + 1, int32(S.Pos.X), int32(S.Pos.Y), i,
                                                    i < MarkerNames.Num() ? *MarkerNames[i] : TEXT("?"));
                    break;
                }
            }
        }
    }

    OutSummary = FString::Printf(TEXT("标记=%d 盒=%d 出生点=%d 巡逻点=%d → %s（entityHitbox r=%.1f h=%.1f）%s%s"),
                                 MarkersFound, Centers.Num(), Spawns.Num(), Patrols.Num(), *Path, HitRadius, HitHalfH,
                                 Skipped > 0
                                     ? *FString::Printf(TEXT(" ⚠跳过%d（实例包围盒无效——空Actor只挂标记件？给Mesh或勾bOverrideBounds）"), Skipped)
                                     : TEXT(""),
                                 *SpawnBlocked);
    if (!SpawnBlocked.IsEmpty())
        UE_LOG(LogTemp, Warning,
               TEXT("[烘焙] ⚠ 出生点净空自检不通过：%s——被掩体（含 34cm 膨胀带）压住的出生点会让人机原地不动、子弹先中箱皮；把掩体挪开（箱缘离出生点 ≥70cm）后重新烘焙"),
               *SpawnBlocked);
    return true;
}
