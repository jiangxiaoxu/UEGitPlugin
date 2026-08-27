// Copyright (c) 2026

#include "GitChangedAssetsModel.h"

#include "Misc/Paths.h"

const TCHAR* LexToString(const EGitChangedAssetState InState)
{
	switch (InState)
	{
	case EGitChangedAssetState::Modified:
		return TEXT("Modified");
	case EGitChangedAssetState::Deleted:
		return TEXT("Deleted");
	case EGitChangedAssetState::Added:
		return TEXT("Added");
	case EGitChangedAssetState::Untracked:
		return TEXT("Untracked");
	case EGitChangedAssetState::Renamed:
		return TEXT("Renamed");
	case EGitChangedAssetState::Conflicted:
		return TEXT("Conflicted");
	default:
		return TEXT("Unknown");
	}
}

bool IsGitChangedAssetUassetPath(const FString& InRepositoryRelativePath)
{
	return !InRepositoryRelativePath.IsEmpty()
		&& FPaths::GetExtension(InRepositoryRelativePath, false).Equals(TEXT("uasset"), ESearchCase::IgnoreCase);
}

void FGitChangedAssetEntry::RecomputeBaseRevertEligibility()
{
	bBaseRevertEligible = false;
	bCanRevert = false;
	RevertBlockReason.Reset();

	if (!IsGitChangedAssetUassetPath(RepositoryRelativePath) || AbsoluteFilename.IsEmpty())
	{
		RevertBlockReason = TEXT("Changed Assets only supports individual .uasset files.");
		return;
	}
	if (IsConflicted())
	{
		RevertBlockReason = TEXT("Resolve the Git conflict before reverting this asset.");
		return;
	}
	if (IsRename() && (!IsGitChangedAssetUassetPath(RenameFromRepositoryRelativePath) || RenameFromAbsoluteFilename.IsEmpty()))
	{
		RevertBlockReason = TEXT("The renamed asset does not have a valid .uasset source path.");
		return;
	}

	bBaseRevertEligible = true;
	// 元数据解析完成前不允许 UI 发起事务, 以保证 OFPA owner 无法解析时不会短暂变为可回退.
	RevertBlockReason = TEXT("Asset metadata is still resolving.");
}
