// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Sightline 自定义日志分类（全模块统一，弃用 LogTemp）
 * 关键事件=Log · 可恢复异常=Warning · 不可恢复=Error · 心跳/收发明细=Verbose（调试时开启）
 */
DECLARE_LOG_CATEGORY_EXTERN(LogSightline, Log, All);
