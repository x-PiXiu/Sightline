#include "Game/SightlineBakeLib.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Game/SightlineOccluderComponent.h"
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
    }
    if (MarkersFound == 0) {
        OutSummary = TEXT("目标世界里没有标记件实例——BP 资产加了组件≠关卡有实例（拖进当前关卡）；PIE 会话里新摆的墙要先重进 PIE");
        return false;
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
    OutSummary = FString::Printf(TEXT("标记=%d 盒=%d → %s（entityHitbox r=%.1f h=%.1f）%s"),
                                 MarkersFound, Centers.Num(), *Path, HitRadius, HitHalfH,
                                 Skipped > 0
                                     ? *FString::Printf(TEXT(" ⚠跳过%d（实例包围盒无效——空Actor只挂标记件？给Mesh或勾bOverrideBounds）"), Skipped)
                                     : TEXT(""));
    return true;
}
