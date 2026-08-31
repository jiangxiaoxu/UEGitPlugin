// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"

class AActor;
class UTypedElementSelectionSet;

/** Standalone Git History window 表示的 Editor 对象类型. */
enum class EGitHistoryTargetKind : uint8
{
	Asset,
	ExternalActor
};

/** 与 History target 兼容的 UI Diff 实现. */
enum class EGitHistoryDiffStrategy : uint8
{
	AssetTools,
	ActorDetails
};

/** OFPA actor History 请求在 selection 时捕获的不可变身份. */
struct FGitActorHistoryTarget
{
	FString Filename;
	TWeakObjectPtr<AActor> Actor;
};

namespace GitSourceControlActorHistory
{
	/** 由 actor external package 的当前 package name 解析 canonical 磁盘文件, 并拒绝未保存或重命名中的身份不一致状态. */
	GITSOURCECONTROL_API bool ResolveExternalActorPackageFilename(const AActor& InActor, FString& OutFilename, FString& OutFailureReason);

	/** 解析唯一的已加载、已保存 Editor-world OFPA main actor; 不执行 Git, 可在菜单生成期调用. */
	GITSOURCECONTROL_API bool ResolveExternalActorHistoryTarget(const UTypedElementSelectionSet& InSelection,
		FGitActorHistoryTarget& OutTarget, FString& OutFailureReason);

	/** 仅从已加载 package 重新解析当前 OFPA main actor, 不隐式加载磁盘 package. */
	GITSOURCECONTROL_API AActor* FindLoadedExternalActorForHistoryDiff(const FString& InCanonicalFilename);

	GITSOURCECONTROL_API EGitHistoryDiffStrategy GetDiffStrategy(EGitHistoryTargetKind InTargetKind);
}
