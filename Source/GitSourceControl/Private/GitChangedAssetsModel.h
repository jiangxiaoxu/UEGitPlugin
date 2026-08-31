// Copyright (c) 2026
//
// Changed Assets 的无 UI 数据模型. UI 行只描述 primary package, Git 事务保留 package artifact closure.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

enum class EGitChangedAssetState : uint8
{
	Modified,
	Deleted,
	Added,
	Untracked,
	Renamed,
	Conflicted,
};

/** 一个 package artifact 的角色. Sidecar 不单独显示为 Changed Assets 行, 但必须进入选中 package 的 Git 事务. */
enum class EGitChangedAssetArtifactKind : uint8
{
	PrimaryPackage,
	Sidecar,
};

/** Git package transaction contract. The same artifact transport intentionally has different state eligibility per user operation. */
enum class EGitChangedAssetOperationMode : uint8
{
	/** Changed Assets Revert to pinned HEAD; selected added, untracked and renamed packages are intentionally supported. */
	ChangedAssetsRevert,
	/** Discard tracked package changes only; never deletes an added or untracked package artifact. */
	DiscardTracked,
	/** Explicit historical/force restore; current Git status does not limit the selected exact package. */
	HistoricalRestore,
};

/**
 * porcelain-v2 中的一个精确 package artifact, 或 selection 时补入的无变更 sibling.
 * 所有 Git mutation、index/fingerprint/backup/rollback 均使用此类型, 不能退化为目录 pathspec.
 */
struct FGitChangedAssetArtifact
{
	FString RepositoryRelativePath;
	FString AbsoluteFilename;
	FString RenameFromRepositoryRelativePath;
	FString RenameFromAbsoluteFilename;
	EGitChangedAssetState State = EGitChangedAssetState::Modified;
	TCHAR IndexStatus = TEXT('.');
	TCHAR WorktreeStatus = TEXT('.');
	bool bHasUntrackedReplacement = false;
	bool bKnownUnchanged = false;
	EGitChangedAssetArtifactKind Kind = EGitChangedAssetArtifactKind::PrimaryPackage;

	bool IsRename() const { return !bKnownUnchanged && State == EGitChangedAssetState::Renamed; }
	bool IsConflicted() const { return !bKnownUnchanged && State == EGitChangedAssetState::Conflicted; }
};

enum class EGitChangedAssetPackageKind : uint8
{
	Regular,
	ExternalActor,
	ExternalObject,
	Unknown,
};

enum class EGitChangedAssetMetadataSource : uint8
{
	Unresolved,
	/** 当前 worktree .uasset 的 package header; 不依赖全局 Asset Registry 的旧索引. */
	CurrentPackageRegistry,
	HeadPackageRegistry,
	DerivedExternalPath,
};

enum class EGitChangedAssetDataLayerMappingSource : uint8
{
	None,
	Current,
	Head,
};

/** 一个已索引的 WorldDataLayers external actor; 只保存 Asset Registry 值数据, 不持有 UObject. */
struct FGitChangedAssetWorldDataLayersIndexEntry
{
	FString RepositoryRelativePath;
	FAssetData AssetData;
	EGitChangedAssetState State = EGitChangedAssetState::Modified;
	bool bHasCurrentAssetData = false;
	bool bChangedRelativeToHead = false;
	/** rename 跨 owner 时此 current WDL 在其 current owner 的 fixed HEAD 中不存在. */
	bool bKnownAbsentAtHead = false;
};

/** 同一 owner/source 的 Data Layer 映射缓存, 随 snapshot 复制并避免重复查询. */
struct FGitChangedAssetDataLayerMappingCache
{
	TSet<FName> AttemptedIdentifiers;
	TMap<FName, TSet<FString>> FriendlyNameCandidates;
	TMap<FName, FString> ResolvedNames;
	/** 仅 Head source: 已从 fixed HEAD 读取的 WorldDataLayers package paths. */
	TSet<FString> AttemptedWorldDataLayersRepositoryPaths;
	/** 仅 Head source: fixed HEAD metadata 不可用, 因而不得使用 current metadata. */
	TSet<FString> UnavailableWorldDataLayersRepositoryPaths;
	/** 仅 Head source: Added/Untracked WDL 在固定 HEAD 中按定义不存在, 不阻断其他 descriptor. */
	TSet<FString> KnownAbsentWorldDataLayersRepositoryPaths;
	TMap<FString, FAssetData> HeadWorldDataLayersAssetData;
};

/** 单个 owner level 的 snapshot-local WorldDataLayers index 与 source-specific caches. */
struct FGitChangedAssetDataLayerOwnerCache
{
	bool bWorldDataLayersIndexed = false;
	TArray<FGitChangedAssetWorldDataLayersIndexEntry> WorldDataLayers;
	FGitChangedAssetDataLayerMappingCache Current;
	FGitChangedAssetDataLayerMappingCache Head;
};

/** 一个相对固定 HEAD 的单 primary package Git 变更. */
struct FGitChangedAssetEntry
{
	/** 当前 Git path, 始终是 repository-relative normalized filename. */
	FString RepositoryRelativePath;

	/** 当前 worktree path; Deleted 资产也保留这个预期位置. */
	FString AbsoluteFilename;

	/** 仅 Rename 的旧 Git/worktree path. */
	FString RenameFromRepositoryRelativePath;
	FString RenameFromAbsoluteFilename;

	/** 由 metadata resolver 填充的 package/display 字段. */
	FString PackageName;
	FString DisplayName;
	FString OwnerLevel;
	FString ObjectPath;
	/** 纯显示用的 owner/path 文本; 不参与 Git identity 或回退操作. */
	FString DisplayOwnerLevel;
	FString DisplayObjectPath;
	/** Data Layer 的原始 package/object path 列表, 仅用于 Tooltip. */
	FString FullDataLayerNames;
	/** Actor descriptor 中的精确 Data Layer 标识; 用于按 owner WorldDataLayers 无加载解析 private/legacy 短名. */
	TArray<FName> ActorDataLayerIdentifiers;
	/** Actor descriptor 中的精确 External Data Layer asset path, 仅用于显示和 Tooltip. */
	FName ExternalDataLayerAssetPath;
	/** Actor descriptor 的 object name; 与 ActorLabel 分离, 用于稳定构造显示 path. */
	FString ActorObjectName;
	FString AssetType;
	FString MetadataFailureReason;

	EGitChangedAssetState State = EGitChangedAssetState::Modified;
	EGitChangedAssetPackageKind PackageKind = EGitChangedAssetPackageKind::Unknown;
	EGitChangedAssetMetadataSource MetadataSource = EGitChangedAssetMetadataSource::Unresolved;

	/** porcelain-v2 XY 原值; ? 记录为 ??, u 记录为 UU. */
	TCHAR IndexStatus = TEXT('.');
	TCHAR WorktreeStatus = TEXT('.');

	/** 同一路径存在 ? 记录时保留 replacement 信息, 但 tracked record 仍是回退基线. */
	bool bHasUntrackedReplacement = false;

	/** Metadata resolver 用于 OFPA owner 资格检查. */
	bool bOwnerLevelResolved = false;
	bool bMetadataResolved = false;
	bool bHasActorDescriptorMetadata = false;
	EGitChangedAssetDataLayerMappingSource DataLayerMappingSource = EGitChangedAssetDataLayerMappingSource::None;

	/** Core 资格不依赖 Asset Registry; 最终资格由 metadata/mutation 再收紧. */
	bool bBaseRevertEligible = false;
	bool bCanRevert = false;
	FString RevertBlockReason;

	/** 只评估单文件 Git 拓扑和 conflict. 不覆盖 metadata resolver 的阻断原因. */
	void RecomputeBaseRevertEligibility();

	bool IsRename() const { return State == EGitChangedAssetState::Renamed; }
	bool IsConflicted() const { return State == EGitChangedAssetState::Conflicted; }
};

/** 一次 repository-wide status 读取的 immutable Git 基线. */
struct FGitChangedAssetSnapshot
{
	FString GitBinary;
	FString RepositoryRoot;
	FString PinnedHead;
	uint64 Generation = 0;
	FDateTime CapturedAtUtc;
	double StatusDurationSeconds = 0.0;
	TArray<FGitChangedAssetEntry> Entries;
	/** 所有 primary/sidecar porcelain records; sidecar 不得因 UI filtering 丢失. */
	TArray<FGitChangedAssetArtifact> ArtifactEntries;
	/** Owner-level WorldDataLayers data, cache and HEAD safety state; lifetime is one refresh snapshot. */
	TMap<FString, FGitChangedAssetDataLayerOwnerCache> DataLayerOwnerCaches;
};

/** 一次 selection-driven Changed Assets Git 事务. 所有 Artifact 都是明确文件, 而非目录或通配 pathspec. */
struct FGitChangedAssetMutationSet
{
	FString PinnedHead;
	EGitChangedAssetOperationMode OperationMode = EGitChangedAssetOperationMode::ChangedAssetsRevert;
	TArray<FGitChangedAssetEntry> SelectedEntries;
	TArray<FGitChangedAssetArtifact> Artifacts;
	FString Signature;

	bool IsEmpty() const { return SelectedEntries.IsEmpty() || Artifacts.IsEmpty(); }
};

GITSOURCECONTROL_API const TCHAR* LexToString(EGitChangedAssetState InState);
/** 仅 OFPA/WDL metadata 路径使用; 不要用它筛掉 .umap. */
GITSOURCECONTROL_API bool IsGitChangedAssetUassetPath(const FString& InRepositoryRelativePath);
GITSOURCECONTROL_API bool IsGitChangedAssetMapPath(const FString& InRepositoryRelativePath);
GITSOURCECONTROL_API bool IsGitChangedAssetPrimaryPackagePath(const FString& InRepositoryRelativePath);
GITSOURCECONTROL_API bool IsGitChangedAssetSidecarPath(const FString& InRepositoryRelativePath);
GITSOURCECONTROL_API bool IsGitChangedAssetArtifactPath(const FString& InRepositoryRelativePath);
