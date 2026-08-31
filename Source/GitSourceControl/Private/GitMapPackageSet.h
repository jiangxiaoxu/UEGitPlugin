// Copyright (c) 2026
//
// Explicit package-artifact and world-lifecycle closures for Changed Assets maps.

#pragma once

#include "CoreMinimal.h"
#include "GitChangedAssetsModel.h"

/** Resolution result for a package target that may be deleted from the worktree. */
struct FGitChangedPrimaryPackageTarget
{
	FString PackageName;
	FString RepositoryRelativePath;
	FString AbsoluteFilename;
	bool bIsMap = false;
	bool bExistsInWorktree = false;
	bool bExistsAtPinnedHead = false;
};

/** Engine-resolved map world roots used only to guard Editor lifecycle transitions. */
struct FGitMapWorldClosure
{
	FString MapPackageName;
	TArray<FString> ExternalActorRoots;
	TArray<FString> ExternalObjectRoots;
	FString Signature;
};

/** Exact package artifacts present in one fixed Git revision. */
struct FGitPackageRevisionArtifactSet
{
	FString Revision;
	FGitChangedPrimaryPackageTarget PrimaryTarget;
	TArray<FString> RepositoryRelativePaths;
	bool bComplete = false;
};

/** Published historical package files. Caller owns session-lifetime cleanup of TemporaryDirectory. */
struct FGitPackageRevisionMaterialization
{
	FString TemporaryDirectory;
	FString PrimaryFilename;
	TArray<FString> ArtifactFilenames;
	TMap<FString, FString> FilenameByRepositoryRelativePath;
};

namespace GitMapPackageSet
{
	/**
	 * Builds one atomic, selection-driven artifact set. Every selected primary
	 * package includes its exact existing/status-visible sidecars; unrelated map
	 * packages, BuiltData and OFPA packages are deliberately not added.
	 */
	GITSOURCECONTROL_API bool BuildSelectionMutationSet(const FGitChangedAssetSnapshot& InSnapshot,
		const TArray<FGitChangedAssetEntry>& InSelectedEntries, EGitChangedAssetOperationMode InOperationMode,
		FGitChangedAssetMutationSet& OutSet, FString& OutError);

	/**
	 * Standalone variant for Menu/Local workflows. It captures a fixed status
	 * snapshot and resolves exact selected primary filenames without defaulting
	 * deleted maps to .uasset. It does not fabricate OFPA owner metadata.
	 */
	GITSOURCECONTROL_API bool BuildSelectionMutationSetForFiles(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FString& InExpectedPinnedHead, const TArray<FString>& InPrimaryFilenames, EGitChangedAssetOperationMode InOperationMode,
		FGitChangedAssetMutationSet& OutSet, FString& OutError);

	/**
	 * GameThread-only metadata bridge for standalone selected OFPA packages.
	 * It resolves a unique owner through Engine external roots, then makes the
	 * mutation set eligible for the shared map+OFPA lifecycle transaction.
	 */
	GITSOURCECONTROL_API bool EnrichMutationSetForLifecycle(FGitChangedAssetMutationSet& InOutSet, FString& OutError);

	/**
	 * Resolve .umap/.uasset without defaulting to .uasset when the workspace file
	 * was deleted. The optional pinned revision is queried with an exact literal
	 * object path. Ambiguous extension identity is rejected.
	 */
	GITSOURCECONTROL_API bool ResolvePrimaryPackageTarget(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FString& InLongPackageName, const FString& InPinnedHead, FGitChangedPrimaryPackageTarget& OutTarget, FString& OutError);

	/**
	 * GameThread-only map closure for lifecycle safety. It uses Engine external
	 * root providers and all initialized containers belonging to this live world;
	 * it never broadens a Git mutation set.
	 */
	GITSOURCECONTROL_API bool BuildMapWorldClosure(const FGitChangedAssetEntry& InMapEntry,
		FGitMapWorldClosure& OutClosure, FString& OutError);

	/** Validate and enumerate the selected package header plus its same-stem sidecars in one fixed revision. */
	GITSOURCECONTROL_API bool BuildPackageRevisionArtifactSet(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FString& InRevision, const FGitChangedPrimaryPackageTarget& InPrimaryTarget, FGitPackageRevisionArtifactSet& OutSet, FString& OutError);

	/**
	 * Confirmation-gated historical restore preflight. It may fetch an exact LFS
	 * primary blob, then validates its Unreal package header. Do not call before
	 * the user confirms Restore.
	 */
	GITSOURCECONTROL_API bool ValidatePackageRevisionPrimaryForRestore(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FGitPackageRevisionArtifactSet& InRevisionSet, FString& OutError);

	/**
	 * Materialize a complete fixed-revision package group to one unique temporary
	 * stem. LFS blobs are locally verified/fetched before publication. On failure
	 * every partial output is deleted; success transfers cleanup ownership to caller.
	 */
	GITSOURCECONTROL_API bool MaterializePackageRevisionArtifacts(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FGitPackageRevisionArtifactSet& InRevisionSet, FGitPackageRevisionMaterialization& OutMaterialization, FString& OutError);
}
