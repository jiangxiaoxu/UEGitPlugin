// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"

#include "GitSourceControlHistoryMode.generated.h"

/** 定义本地 Git 资产历史如何追踪已提交路径. */
UENUM(BlueprintType)
enum class EGitLocalSourceControlHistoryMode : uint8
{
	/** 只查询资产当前 repository path. */
	CurrentPath,

	/** 只追踪已提交, single-parent, byte-identical 的 Git rename. */
	ExactRenames,
};
