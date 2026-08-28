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

/**
 * Fixed-HEAD package payload prepared by a worker and decoded on the Game Thread.
 *
 * This intentionally contains no Asset Registry/UObject state: the worker only runs
 * one cat-file batch and hands its plain bytes back to the Game Thread.
 */
struct FGitChangedAssetHeadMetadataResult
{
	int32 EntryIndex = INDEX_NONE;
	/** 非空时此结果是 owner WorldDataLayers 的 fixed-HEAD metadata, 不是 Changed Asset 行。 */
	FString DataLayerOwnerLevel;
	FString WorldDataLayersRepositoryRelativePath;
	EGitChangedAssetState WorldDataLayersState = EGitChangedAssetState::Modified;
	/** Archive identity retained for package-header decoding, including LFS objects without a .uasset extension. */
	FString LogicalFilename;
	/** Exact git blob bytes, possibly a Git LFS pointer. Decoded only on the Game Thread. */
	TArray<uint8> BlobData;
	/** Snapshot-worker-resolved local LFS object. Empty means no local object was available. */
	FString LocalLfsObjectFilename;
	FString FailureReason;

	bool IsWorldDataLayersMetadata() const { return !DataLayerOwnerLevel.IsEmpty(); }
};

/**
 * 将 Changed Assets 的 Git 路径映射为 Editor 可显示的资产/OFPA metadata.
 *
 * Current metadata 直接读取 worktree package header, 不写入全局 Asset Registry, 必须在 Game Thread 调用. HEAD-only Git
 * blob read 可在 background worker 调用, 但 payload 的 package-header decode/显示映射
 * 必须在 Game Thread 调用; 调用方负责以 Snapshot generation 丢弃过期结果.
 */
class GITSOURCECONTROL_API FGitChangedAssetsMetadataResolver final
{
public:
	/**
	 * 同步完成 current metadata, 仅供兼容的非增量调用者和测试使用.
	 */
	static void ResolveCurrentMetadata(FGitChangedAssetSnapshot& InOutSnapshot);

	/** 初始化 current worktree package-header metadata 阶段, 但不发布 snapshot。 */
	static void BeginCurrentMetadata(FGitChangedAssetSnapshot& InOutSnapshot);

	/** 在 Game Thread 应用 current metadata range. 每行最多读取一个 package header, 不可抢占。 */
	static int32 ApplyCurrentMetadataRange(FGitChangedAssetSnapshot& InOutSnapshot, int32 InStartIndex, int32 InMaxCount);

	/** 在全部 current range 完成后建立 current-only Data Layer 映射和回退资格。 */
	static void FinalizeCurrentMetadata(FGitChangedAssetSnapshot& InOutSnapshot);

	/**
	 * 读取当前 worktree 中不存在, 但固定 HEAD 中存在的 .uasset Git blob. 仅产生
	 * plain payload, 不调用 Asset Registry/ActorDesc/UObject API. 每次调用最多启动一个本地
	 * `git cat-file --batch-command -Z` process; 仅在 worker 解析 pointer 并查询本地 object, 从不 fetch 或启动 Git LFS.
	 */
	static bool ResolveHeadOnlyMetadata(const FGitChangedAssetSnapshot& InSnapshot,
		TArray<FGitChangedAssetHeadMetadataResult>& OutResults, FString& OutError,
		TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> InCancellationContext = nullptr);

	/**
	 * 在 Game Thread 应用一个 HEAD payload range, 但不执行全 snapshot 的 Data Layer resolve/eligibility finalize.
	 * 返回实际处理的 payload 数量, 供 UI coordinator 在 tick budget 内推进。
	 */
	static int32 ApplyHeadOnlyMetadataRange(FGitChangedAssetSnapshot& InOutSnapshot,
		const TArray<FGitChangedAssetHeadMetadataResult>& InResults, int32 InStartIndex, int32 InMaxCount);

	/** 在所有 HEAD payload range 应用完毕后, 一次性完成 Data Layer resolve 和 revert eligibility。 */
	static void FinalizeHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot);

	/** 在 Game Thread 同步应用全部 HEAD payload, 供非增量调用者和测试使用。 */
	static void ApplyHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot, const TArray<FGitChangedAssetHeadMetadataResult>& InResults);

	/**
	 * 在 current/HEAD metadata 均已完成后, 为剩余 OFPA 行执行昂贵的 World root fallback.
	 * 必须在 Game Thread 调用; 正常的 OptionalOuterPath fast path 不会进入这里.
	 */
	static void ResolveOutstandingOwnerFallback(FGitChangedAssetSnapshot& InOutSnapshot);
};

#if WITH_DEV_AUTOMATION_TESTS
class FWorldPartitionActorDesc;

namespace GitChangedAssetsMetadataTesting
{
	/** Exercise the same no-load package-header fallback used after an Asset Registry miss. */
	GITSOURCECONTROL_API bool ApplyPackageHeaderMetadata(const FString& InFilename, FGitChangedAssetEntry& InOutEntry, FString& OutFailureReason);
	/** Validate filename-to-package mapping without mutating the Git/filesystem identity. */
	GITSOURCECONTROL_API bool EnsurePackageName(FGitChangedAssetEntry& InOutEntry);
	/** Pure source selection for fixed-HEAD metadata requests. */
	GITSOURCECONTROL_API bool GetHeadMetadataSourcePath(const FGitChangedAssetEntry& InEntry, bool bCurrentFilenameExists,
		FString& OutRepositoryRelativePath, FString& OutLogicalFilename);
	/** Deleted entries must never consume current worktree metadata, including replacement files. */
	GITSOURCECONTROL_API bool ShouldResolveCurrentFileMetadata(const FGitChangedAssetEntry& InEntry);
	/** Exercise fixed-HEAD blob/LFS package-header metadata extraction. */
	GITSOURCECONTROL_API bool LoadHeadMetadataBlob(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InLogicalFilename,
		const TArray<uint8>& InBlobData, TArray<FAssetData>& OutAssetData, FString& OutFailureReason);

	/** Apply plain Asset Registry data through the production display/OFPA mapping path. */
	GITSOURCECONTROL_API void ApplyAssetData(const FAssetData& InAssetData, FGitChangedAssetEntry& InOutEntry);

	/** Pure display helpers used to validate Data Layer naming without loading packages. */
	GITSOURCECONTROL_API FString FriendlyDataLayerName(const FString& InDataLayerPath);
	GITSOURCECONTROL_API FString BuildActorDisplayObjectPath(const FString& InObjectPath, const FString& InActorName,
		const TArray<FString>& InFriendlyDataLayerNames);
	GITSOURCECONTROL_API bool RequiresWorldDataLayersDescriptor(FName InDataLayerIdentifier);
	GITSOURCECONTROL_API void ApplyResolvedDataLayerNames(FGitChangedAssetEntry& InOutEntry, const TMap<FName, FString>& InResolvedNames);
	GITSOURCECONTROL_API void ApplySourceSpecificDataLayerNames(FGitChangedAssetEntry& InOutEntry,
		const TMap<FName, FString>& InCurrentNames, const TMap<FName, FString>& InHeadNames);
	GITSOURCECONTROL_API void SeedHeadOnlyWorldDataLayersIndex(FGitChangedAssetSnapshot& InOutSnapshot, const FString& InOwnerLevel,
		const FString& InHeadRepositoryRelativePath, EGitChangedAssetState InState);
	GITSOURCECONTROL_API bool HasCompleteHeadWorldDataLayersMetadata(const FGitChangedAssetDataLayerOwnerCache& InOwnerCache);
	GITSOURCECONTROL_API bool ResolveHeadWorldDataLayersOwnerWithoutOuter(const FGitChangedAssetSnapshot& InSnapshot,
		const FGitChangedAssetEntry& InFallbackEntry, FString& OutOwnerLevel);
	GITSOURCECONTROL_API bool IsUsableWorldDataLayersDescriptor(const FWorldPartitionActorDesc& InActorDesc);
}
#endif
