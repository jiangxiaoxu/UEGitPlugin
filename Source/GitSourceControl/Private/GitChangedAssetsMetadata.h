// Copyright (c) 2026
//
// Changed Assets 的 Asset Registry metadata 解析. 不加载 UObject, 不启动网络 Git/LFS 操作.

#pragma once

#include "GitChangedAssetsModel.h"
#include "AssetRegistry/AssetData.h"

namespace GitSourceControlUtils
{
	class FGitOperationCancellationContext;
}

/** Plain HEAD package metadata prepared by a worker and applied on the Game Thread. */
struct FGitChangedAssetHeadMetadataResult
{
	int32 EntryIndex = INDEX_NONE;
	TArray<FAssetData> AssetData;
	FString FailureReason;
};

/**
 * 将 Changed Assets 的 Git 路径映射为 Editor 可显示的资产/OFPA metadata.
 *
 * Current metadata 会更新全局 Asset Registry, 必须在 Game Thread 调用. HEAD-only
 * metadata 不写 Asset Registry, 可在 background worker 调用; 调用方负责以 Snapshot
 * generation 丢弃过期结果.
 */
class FGitChangedAssetsMetadataResolver final
{
public:
	/**
	 * 批量查询当前存在的 .uasset. 只对 exact Asset Registry miss 执行一次 scoped scan.
	 * 此函数同时为 metadata 缺失的 OFPA 建立一次 snapshot-local owner map fallback.
	 */
	static void ResolveCurrentMetadata(FGitChangedAssetSnapshot& InOutSnapshot);

	/**
	 * 解析当前 worktree 中不存在, 但固定 HEAD 中存在的 .uasset header. 只产生
	 * plain FAssetData, 不调用 ActorDesc/UObject API. 每次调用最多启动一个本地
	 * `git cat-file --batch-command -Z` process; 仅使用已存在的 LFS object, 从不 fetch.
	 */
	static bool ResolveHeadOnlyMetadata(const FGitChangedAssetSnapshot& InSnapshot,
		TArray<FGitChangedAssetHeadMetadataResult>& OutResults, FString& OutError,
		TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> InCancellationContext = nullptr);

	/** 在 Game Thread 将 worker 得到的 HEAD package metadata 转换为显示字段和 OFPA descriptor. */
	static void ApplyHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot, const TArray<FGitChangedAssetHeadMetadataResult>& InResults);

	/**
	 * 在 current/HEAD metadata 均已完成后, 为剩余 OFPA 行执行昂贵的 World root fallback.
	 * 必须在 Game Thread 调用; 正常的 OptionalOuterPath fast path 不会进入这里.
	 */
	static void ResolveOutstandingOwnerFallback(FGitChangedAssetSnapshot& InOutSnapshot);
};

#if WITH_DEV_AUTOMATION_TESTS
namespace GitChangedAssetsMetadataTesting
{
	/** Exercise the same no-load package-header fallback used after an Asset Registry miss. */
	GITSOURCECONTROL_API bool ApplyPackageHeaderMetadata(const FString& InFilename, FGitChangedAssetEntry& InOutEntry, FString& OutFailureReason);

	/** Apply plain Asset Registry data through the production display/OFPA mapping path. */
	GITSOURCECONTROL_API void ApplyAssetData(const FAssetData& InAssetData, FGitChangedAssetEntry& InOutEntry);
}
#endif
