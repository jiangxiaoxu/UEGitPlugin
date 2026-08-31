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

bool IsGitChangedAssetMapPath(const FString& InRepositoryRelativePath)
{
	return !InRepositoryRelativePath.IsEmpty()
		&& FPaths::GetExtension(InRepositoryRelativePath, false).Equals(TEXT("umap"), ESearchCase::IgnoreCase);
}

bool IsGitChangedAssetPrimaryPackagePath(const FString& InRepositoryRelativePath)
{
	return IsGitChangedAssetUassetPath(InRepositoryRelativePath) || IsGitChangedAssetMapPath(InRepositoryRelativePath);
}

bool IsGitChangedAssetSidecarPath(const FString& InRepositoryRelativePath)
{
	if (InRepositoryRelativePath.IsEmpty())
	{
		return false;
	}
	const FString Extension = FPaths::GetExtension(InRepositoryRelativePath, false);
	return Extension.Equals(TEXT("uexp"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT("ubulk"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT("uptnl"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT("upayload"), ESearchCase::IgnoreCase);
}

bool IsGitChangedAssetArtifactPath(const FString& InRepositoryRelativePath)
{
	return IsGitChangedAssetPrimaryPackagePath(InRepositoryRelativePath) || IsGitChangedAssetSidecarPath(InRepositoryRelativePath);
}

void FGitChangedAssetEntry::RecomputeBaseRevertEligibility()
{
	bBaseRevertEligible = false;
	bCanRevert = false;
	RevertBlockReason.Reset();

	if (!IsGitChangedAssetPrimaryPackagePath(RepositoryRelativePath) || AbsoluteFilename.IsEmpty())
	{
		RevertBlockReason = TEXT("Changed Assets only supports individual .uasset or .umap primary packages.");
		return;
	}
	if (IsConflicted())
	{
		RevertBlockReason = TEXT("Resolve the Git conflict before reverting this asset.");
		return;
	}
	if (IsRename() && (!IsGitChangedAssetPrimaryPackagePath(RenameFromRepositoryRelativePath) || RenameFromAbsoluteFilename.IsEmpty()))
	{
		RevertBlockReason = TEXT("The renamed asset does not have a valid primary package source path.");
		return;
	}
	if (IsRename() && IsGitChangedAssetMapPath(RepositoryRelativePath) != IsGitChangedAssetMapPath(RenameFromRepositoryRelativePath))
	{
		RevertBlockReason = TEXT("Changed Assets does not support a rename between .umap and .uasset package kinds.");
		return;
	}

	bBaseRevertEligible = true;
	// 元数据解析完成前不允许 UI 发起事务, 以保证 OFPA owner 无法解析时不会短暂变为可回退.
	RevertBlockReason = TEXT("Asset metadata is still resolving.");
}
