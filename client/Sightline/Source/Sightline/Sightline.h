// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

/**
 * Sightline 自定义日志分类（全模块统一，弃用 LogTemp）
 * 关键事件=Log · 可恢复异常=Warning · 不可恢复=Error · 心跳/收发明细=Verbose（调试时开启）
 */
DECLARE_LOG_CATEGORY_EXTERN(LogSightline, Log, All);

/** KPI② STAT 采样/同步日志总开关（PIE 控制台输 Sightline.StatLog 0/1 实时切换；默认 1=采集）
 *  消音范围：SELF / PEER / DELAY 全部 [STAT] 行——采集期结束后关掉即消音，曲线数据不受影响 */
extern TAutoConsoleVariable<int32> CSightlineStatLog;
