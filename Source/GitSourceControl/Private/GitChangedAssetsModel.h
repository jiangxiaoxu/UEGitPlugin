// Copyright (c) 2026
//
// Changed Assets 的无 UI 数据模型. 只描述单个 .uasset, 不处理额外文件.

#pragma once

#include "CoreMinimal.h"

enum class EGitChangedAssetState : uint8
{
	Modified,
	Deleted,
	Added,
	Untracked,
	Renamed,
	Conflicted,
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
	CurrentAssetRegistry,
	HeadPackageRegistry,
	DerivedExternalPath,
};

/** 一个相对固定 HEAD 的单 .uasset Git 变更. */
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
};

GITSOURCECONTROL_API const TCHAR* LexToString(EGitChangedAssetState InState);
GITSOURCECONTROL_API bool IsGitChangedAssetUassetPath(const FString& InRepositoryRelativePath);
