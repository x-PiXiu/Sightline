// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sightline.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogSightline);

// KPI② STAT/ANIM 诊断日志总开关（默认关=平时消音；KPI 采集/联调诊断时控制台 Sightline.StatLog 1 开启）
TAutoConsoleVariable<int32> CSightlineStatLog(
    TEXT("Sightline.StatLog"), 0,
    TEXT("STAT 采样/同步日志总开关（0=消音 1=采集）"),
    ECVF_Default);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Sightline, "Sightline");
