// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sightline.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogSightline);

// KPI② STAT 日志总开关（M5 消音件；默认开——采集期结束后 Sightline.StatLog 0 消音）
TAutoConsoleVariable<int32> CSightlineStatLog(
    TEXT("Sightline.StatLog"), 1,
    TEXT("STAT 采样/同步日志总开关（0=消音 1=采集）"),
    ECVF_Default);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Sightline, "Sightline");
