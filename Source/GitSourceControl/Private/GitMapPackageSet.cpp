// Copyright (c) 2026

#include "GitMapPackageSet.h"

#include "GitChangedAssetsStatus.h"
#include "GitLfsLocalObjectStore.h"
#include "GitSourceControlUtils.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "WorldPartition/ActorDescContainerInstance.h"
#include "WorldPartition/WorldPartition.h"

namespace GitMapPackageSetPrivate
{
	FString NormalizePathKey(const FString& InFilename)
	{
		FString Result = InFilename;
		FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
		Result.ToLowerInline();
#endif
		return Result;
	}

	bool MakeRepositoryRelativePath(const FString& InRepositoryRoot, const FString& InAbsoluteFilename, FString& OutRelativePath)
	{
		FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		FPaths::NormalizeDirectoryName(RepositoryRoot);
		if (!RepositoryRoot.EndsWith(TEXT("/")))
		{
			RepositoryRoot += TEXT("/");
		}
		FString AbsoluteFilename = FPaths::ConvertRelativePathToFull(InAbsoluteFilename);
		FPaths::NormalizeFilename(AbsoluteFilename);
		if (!FPaths::IsUnderDirectory(AbsoluteFilename, RepositoryRoot))
		{
			return false;
		}
		OutRelativePath = AbsoluteFilename;
		if (!FPaths::MakePathRelativeTo(OutRelativePath, *RepositoryRoot))
		{
			return false;
		}
		FPaths::NormalizeFilename(OutRelativePath);
		return !OutRelativePath.IsEmpty() && !OutRelativePath.StartsWith(TEXT("../"), ESearchCase::CaseSensitive);
	}

	void AddUniquePath(TArray<FString>& InOutPaths, const FString& InPath)
	{
		if (!InPath.IsEmpty() && !InOutPaths.ContainsByPredicate([&InPath](const FString& Existing)
		{
			return Existing.Equals(InPath, ESearchCase::IgnoreCase);
		}))
		{
			InOutPaths.Add(InPath);
		}
	}

	void AddPackageSidecarCandidates(const FString& InPrimaryFilename, TArray<FString>& OutCandidates)
	{
		if (InPrimaryFilename.IsEmpty())
		{
			return;
		}
		const FString PackageStem = FPaths::ChangeExtension(InPrimaryFilename, FString());
		AddUniquePath(OutCandidates, PackageStem + TEXT(".uexp"));
		AddUniquePath(OutCandidates, PackageStem + TEXT(".ubulk"));
		AddUniquePath(OutCandidates, PackageStem + TEXT(".uptnl"));
		AddUniquePath(OutCandidates, PackageStem + TEXT(".m.ubulk"));
		AddUniquePath(OutCandidates, PackageStem + TEXT(".upayload"));
	}

	const FGitChangedAssetArtifact* FindArtifactByCurrentPath(const FGitChangedAssetSnapshot& InSnapshot, const FString& InAbsoluteFilename)
	{
		const FString Key = NormalizePathKey(InAbsoluteFilename);
		return InSnapshot.ArtifactEntries.FindByPredicate([&Key](const FGitChangedAssetArtifact& Candidate)
		{
			return NormalizePathKey(Candidate.AbsoluteFilename) == Key;
		});
	}

	const FGitChangedAssetArtifact* FindArtifactByRenameSourcePath(const FGitChangedAssetSnapshot& InSnapshot, const FString& InAbsoluteFilename)
	{
		const FString Key = NormalizePathKey(InAbsoluteFilename);
		return InSnapshot.ArtifactEntries.FindByPredicate([&Key](const FGitChangedAssetArtifact& Candidate)
		{
			return Candidate.IsRename() && NormalizePathKey(Candidate.RenameFromAbsoluteFilename) == Key;
		});
	}

	void AddUniqueArtifact(TArray<FGitChangedAssetArtifact>& InOutArtifacts, FGitChangedAssetArtifact InArtifact)
	{
		const FString Key = NormalizePathKey(InArtifact.AbsoluteFilename);
		if (!InOutArtifacts.ContainsByPredicate([&Key](const FGitChangedAssetArtifact& Existing)
		{
			return NormalizePathKey(Existing.AbsoluteFilename) == Key;
		}))
		{
			InOutArtifacts.Add(MoveTemp(InArtifact));
		}
	}

	bool AddArtifactForPath(const FGitChangedAssetSnapshot& InSnapshot, const FString& InAbsoluteFilename,
		const EGitChangedAssetArtifactKind InKind, TArray<FGitChangedAssetArtifact>& InOutArtifacts, FString& OutError)
	{
		if (const FGitChangedAssetArtifact* Existing = FindArtifactByCurrentPath(InSnapshot, InAbsoluteFilename))
		{
			FGitChangedAssetArtifact Artifact = *Existing;
			Artifact.Kind = InKind;
			AddUniqueArtifact(InOutArtifacts, MoveTemp(Artifact));
			return true;
		}

		if (!IFileManager::Get().FileExists(*InAbsoluteFilename))
		{
			return true;
		}

		FGitChangedAssetArtifact Artifact;
		Artifact.AbsoluteFilename = FPaths::ConvertRelativePathToFull(InAbsoluteFilename);
		FPaths::NormalizeFilename(Artifact.AbsoluteFilename);
		if (!MakeRepositoryRelativePath(InSnapshot.RepositoryRoot, Artifact.AbsoluteFilename, Artifact.RepositoryRelativePath))
		{
			OutError = FString::Printf(TEXT("The selected package sidecar is outside the Git repository: %s"), *Artifact.AbsoluteFilename);
			return false;
		}
		Artifact.bKnownUnchanged = true;
		Artifact.Kind = InKind;
		AddUniqueArtifact(InOutArtifacts, MoveTemp(Artifact));
		return true;
	}

	bool AddRenameSourceArtifact(const FGitChangedAssetSnapshot& InSnapshot, const FString& InRenameSourceFilename,
		TArray<FGitChangedAssetArtifact>& InOutArtifacts)
	{
		if (const FGitChangedAssetArtifact* Existing = FindArtifactByRenameSourcePath(InSnapshot, InRenameSourceFilename))
		{
			FGitChangedAssetArtifact Artifact = *Existing;
			Artifact.Kind = EGitChangedAssetArtifactKind::Sidecar;
			AddUniqueArtifact(InOutArtifacts, MoveTemp(Artifact));
			return true;
		}
		if (const FGitChangedAssetArtifact* DeletedSource = FindArtifactByCurrentPath(InSnapshot, InRenameSourceFilename))
		{
			FGitChangedAssetArtifact Artifact = *DeletedSource;
			Artifact.Kind = EGitChangedAssetArtifactKind::Sidecar;
			AddUniqueArtifact(InOutArtifacts, MoveTemp(Artifact));
		}
		return true;
	}

	bool DoesRevisionContainExactPath(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InRevision,
		const FString& InRepositoryRelativePath, bool& bOutExists, FString& OutError)
	{
		bOutExists = false;
		FString IgnoredOutput;
		FString GitError;
		if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("cat-file"), InGitBinary, InRepositoryRoot,
			{ TEXT("-e"), InRevision + TEXT(":") + InRepositoryRelativePath }, {}, IgnoredOutput, GitError, 0, false))
		{
			bOutExists = true;
			return true;
		}
		// cat-file exit 1 is the documented "not found" response. Other launch/protocol failures still have diagnostics.
		if (GitError.Contains(TEXT("Not a valid object name"), ESearchCase::IgnoreCase)
			|| GitError.Contains(TEXT("exists on disk"), ESearchCase::IgnoreCase)
			|| GitError.Contains(TEXT("path '"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (GitError.IsEmpty())
		{
			return true;
		}
		OutError = GitError;
		return false;
	}

	bool MaterializeRevisionLfsPointer(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InRevision,
		const FString& InRepositoryRelativePath, const FString& InFilename, FString& OutError)
	{
		FGitLfsPointer Pointer;
		const EGitLfsPointerParseResult PointerResult = ParseGitLfsPointerFile(InFilename, Pointer);
		if (PointerResult == EGitLfsPointerParseResult::NotPointer)
		{
			return true;
		}
		if (PointerResult == EGitLfsPointerParseResult::InvalidPointer)
		{
			OutError = FString::Printf(TEXT("Historical package artifact contains an invalid Git LFS pointer: %s"), *InRepositoryRelativePath);
			return false;
		}

		FGitLfsLocalObjectStore ObjectStore(InGitBinary, InRepositoryRoot);
		FString ObjectFilename;
		EGitLfsLocalObjectLookupResult LookupResult = ObjectStore.FindObject(Pointer, ObjectFilename, OutError);
		if (LookupResult == EGitLfsLocalObjectLookupResult::Error)
		{
			return false;
		}
		if (LookupResult != EGitLfsLocalObjectLookupResult::Found
			&& !GitSourceControlUtils::FetchLfsContentForRevision(InGitBinary, InRepositoryRoot, InRevision, InRepositoryRelativePath, OutError))
		{
			return false;
		}
		if (LookupResult != EGitLfsLocalObjectLookupResult::Found)
		{
			ObjectStore.InvalidateCachedObject(Pointer);
			LookupResult = ObjectStore.FindObject(Pointer, ObjectFilename, OutError);
		}
		if (LookupResult != EGitLfsLocalObjectLookupResult::Found
			|| !GitSourceControlUtils::VerifyLocalLfsObject(InGitBinary, InRepositoryRoot, ObjectFilename, Pointer.Oid, Pointer.Size, OutError)
			|| IFileManager::Get().Copy(*InFilename, *ObjectFilename, true, true) != COPY_OK)
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Could not materialize the local Git LFS object for historical package artifact: %s"), *InRepositoryRelativePath);
			}
			return false;
		}
		return true;
	}

	bool ValidateHistoricalPrimaryPackage(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InRevision,
		const FString& InRepositoryRelativePath, FString& OutError)
	{
		const FString TemporaryFilename = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(), TEXT("git-source-control-package-header-"), TEXT(".tmp"));
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*TemporaryFilename, false, true, true); };
		if (!GitSourceControlUtils::DumpRevisionBlobToFile(InGitBinary, InRepositoryRoot,
			InRevision + TEXT(":") + InRepositoryRelativePath, TemporaryFilename, OutError))
		{
			return false;
		}
		if (!MaterializeRevisionLfsPointer(InGitBinary, InRepositoryRoot, InRevision, InRepositoryRelativePath, TemporaryFilename, OutError))
		{
			return false;
		}
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *TemporaryFilename) || Data.Num() < sizeof(uint32))
		{
			OutError = TEXT("The historical primary Unreal package is empty or unreadable.");
			return false;
		}
		const uint32 Tag = *reinterpret_cast<const uint32*>(Data.GetData());
		if (Tag != PACKAGE_FILE_TAG && Tag != PACKAGE_FILE_TAG_SWAPPED)
		{
			OutError = TEXT("The historical primary revision is not a valid Unreal package.");
			return false;
		}
		return true;
	}

	bool ResolveMapPackageName(const FGitChangedAssetEntry& InMapEntry, FString& OutMapPackageName, FString& OutError)
	{
		OutMapPackageName = InMapEntry.PackageName;
		if (OutMapPackageName.IsEmpty() && !FPackageName::TryConvertFilenameToLongPackageName(InMapEntry.AbsoluteFilename, OutMapPackageName))
		{
			OutError = FString::Printf(TEXT("Could not resolve the selected map package name: %s"), *InMapEntry.AbsoluteFilename);
			return false;
		}
		return true;
	}

	bool ResolveStandaloneExternalOwner(FGitChangedAssetEntry& InOutEntry, FString& OutError)
	{
		const bool bExternalActor = InOutEntry.RepositoryRelativePath.Contains(TEXT("/__ExternalActors__/"), ESearchCase::IgnoreCase);
		const bool bExternalObject = InOutEntry.RepositoryRelativePath.Contains(TEXT("/__ExternalObjects__/"), ESearchCase::IgnoreCase);
		if (!bExternalActor && !bExternalObject)
		{
			return true;
		}
		FString ExternalPackageName = InOutEntry.PackageName;
		if (ExternalPackageName.IsEmpty() && !FPackageName::TryConvertFilenameToLongPackageName(InOutEntry.AbsoluteFilename, ExternalPackageName))
		{
			OutError = FString::Printf(TEXT("Could not resolve the selected external package name: %s"), *InOutEntry.AbsoluteFilename);
			return false;
		}

		FARFilter WorldFilter;
		WorldFilter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
		WorldFilter.bRecursiveClasses = true;
		WorldFilter.bIncludeOnlyOnDiskAssets = true;
		TArray<FAssetData> WorldAssets;
		IAssetRegistry::GetChecked().GetAssets(WorldFilter, WorldAssets, false);
		TSet<FString> CandidateOwners;
		for (const FAssetData& WorldAsset : WorldAssets)
		{
			const FString CandidateOwner = WorldAsset.PackageName.ToString();
			const TArray<FString> Roots = bExternalObject
				? ULevel::GetExternalObjectsPaths(CandidateOwner)
				: ULevel::GetExternalActorsPaths(CandidateOwner);
			if (Roots.ContainsByPredicate([&ExternalPackageName](const FString& Root)
			{
				return !Root.IsEmpty() && ExternalPackageName.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
			}))
			{
				CandidateOwners.Add(CandidateOwner);
			}
		}
		if (CandidateOwners.Num() != 1)
		{
			OutError = CandidateOwners.IsEmpty()
				? FString::Printf(TEXT("Could not resolve a unique owner map for selected OFPA package: %s"), *InOutEntry.AbsoluteFilename)
				: FString::Printf(TEXT("Selected OFPA package matches multiple owner maps: %s"), *InOutEntry.AbsoluteFilename);
			return false;
		}
		InOutEntry.PackageName = MoveTemp(ExternalPackageName);
		InOutEntry.PackageKind = bExternalActor ? EGitChangedAssetPackageKind::ExternalActor : EGitChangedAssetPackageKind::ExternalObject;
		InOutEntry.OwnerLevel = CandidateOwners.Array()[0];
		InOutEntry.bOwnerLevelResolved = true;
		InOutEntry.bMetadataResolved = true;
		InOutEntry.bCanRevert = InOutEntry.bBaseRevertEligible;
		InOutEntry.RevertBlockReason.Reset();
		return true;
	}

	void SortAndSignature(FGitChangedAssetMutationSet& InOutSet)
	{
		InOutSet.Artifacts.Sort([](const FGitChangedAssetArtifact& Left, const FGitChangedAssetArtifact& Right)
		{
			return Left.RepositoryRelativePath < Right.RepositoryRelativePath;
		});
		TArray<FString> Tokens;
		Tokens.Reserve(InOutSet.Artifacts.Num() + 1);
		Tokens.Add(FString::Printf(TEXT("HEAD:%s|Mode:%d"), *InOutSet.PinnedHead, static_cast<int32>(InOutSet.OperationMode)));
		for (const FGitChangedAssetArtifact& Artifact : InOutSet.Artifacts)
		{
			Tokens.Add(FString::Printf(TEXT("%s|%s|%d|%d,%d|%d|%s"), *Artifact.RepositoryRelativePath,
				*Artifact.RenameFromRepositoryRelativePath, static_cast<int32>(Artifact.State), static_cast<int32>(Artifact.IndexStatus), static_cast<int32>(Artifact.WorktreeStatus),
				Artifact.bKnownUnchanged ? 1 : 0, LexToString(Artifact.State)));
		}
		InOutSet.Signature = FString::Join(Tokens, TEXT("\n"));
	}
}

bool GitMapPackageSet::BuildSelectionMutationSet(const FGitChangedAssetSnapshot& InSnapshot,
	const TArray<FGitChangedAssetEntry>& InSelectedEntries, const EGitChangedAssetOperationMode InOperationMode,
	FGitChangedAssetMutationSet& OutSet, FString& OutError)
{
	OutSet = FGitChangedAssetMutationSet();
	OutError.Reset();
	if (InSnapshot.PinnedHead.IsEmpty() || InSnapshot.RepositoryRoot.IsEmpty() || InSelectedEntries.IsEmpty())
	{
		OutError = TEXT("Changed Assets selection requires a complete pinned snapshot and one or more primary packages.");
		return false;
	}

	OutSet.PinnedHead = InSnapshot.PinnedHead;
	OutSet.OperationMode = InOperationMode;
	TSet<FString> SelectedPrimaryKeys;
	for (const FGitChangedAssetEntry& SelectedEntry : InSelectedEntries)
	{
		if (!IsGitChangedAssetPrimaryPackagePath(SelectedEntry.RepositoryRelativePath) || SelectedEntry.AbsoluteFilename.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Changed Assets selection contains a non-primary package: %s"), *SelectedEntry.RepositoryRelativePath);
			return false;
		}
		const FString PrimaryKey = GitMapPackageSetPrivate::NormalizePathKey(SelectedEntry.AbsoluteFilename);
		if (SelectedPrimaryKeys.Contains(PrimaryKey))
		{
			OutError = FString::Printf(TEXT("Changed Assets selection contains the same package more than once: %s"), *SelectedEntry.AbsoluteFilename);
			return false;
		}
		SelectedPrimaryKeys.Add(PrimaryKey);
		OutSet.SelectedEntries.Add(SelectedEntry);

		if (!GitMapPackageSetPrivate::AddArtifactForPath(InSnapshot, SelectedEntry.AbsoluteFilename, EGitChangedAssetArtifactKind::PrimaryPackage, OutSet.Artifacts, OutError))
		{
			return false;
		}

		TArray<FString> CurrentSidecars;
		GitMapPackageSetPrivate::AddPackageSidecarCandidates(SelectedEntry.AbsoluteFilename, CurrentSidecars);
		for (const FString& Sidecar : CurrentSidecars)
		{
			if (!GitMapPackageSetPrivate::AddArtifactForPath(InSnapshot, Sidecar, EGitChangedAssetArtifactKind::Sidecar, OutSet.Artifacts, OutError))
			{
				return false;
			}
		}

		if (SelectedEntry.IsRename())
		{
			TArray<FString> RenameSourceSidecars;
			GitMapPackageSetPrivate::AddPackageSidecarCandidates(SelectedEntry.RenameFromAbsoluteFilename, RenameSourceSidecars);
			for (const FString& Sidecar : RenameSourceSidecars)
			{
				GitMapPackageSetPrivate::AddRenameSourceArtifact(InSnapshot, Sidecar, OutSet.Artifacts);
			}
		}
	}

	if (OutSet.Artifacts.IsEmpty())
	{
		OutError = TEXT("Changed Assets selection has no exact package artifacts.");
		return false;
	}
	GitMapPackageSetPrivate::SortAndSignature(OutSet);
	return true;
}

bool GitMapPackageSet::ResolvePrimaryPackageTarget(const FString& InGitBinary, const FString& InRepositoryRoot,
	const FString& InLongPackageName, const FString& InPinnedHead, FGitChangedPrimaryPackageTarget& OutTarget, FString& OutError)
{
	OutTarget = FGitChangedPrimaryPackageTarget();
	OutError.Reset();
	if (!FPackageName::IsValidLongPackageName(InLongPackageName, false) || InGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty())
	{
		OutError = TEXT("A valid long package name, Git binary, and repository root are required.");
		return false;
	}

	struct FCandidate
	{
		FString Filename;
		FString RepositoryRelativePath;
		bool bIsMap = false;
		bool bInWorktree = false;
		bool bAtRevision = false;
	};
	TArray<FCandidate> Candidates;
	for (const bool bIsMap : { true, false })
	{
		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.bIsMap = bIsMap;
		Candidate.Filename = FPackageName::LongPackageNameToFilename(InLongPackageName,
			bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		Candidate.bInWorktree = IFileManager::Get().FileExists(*Candidate.Filename);
		if (!GitMapPackageSetPrivate::MakeRepositoryRelativePath(InRepositoryRoot, Candidate.Filename, Candidate.RepositoryRelativePath))
		{
			OutError = FString::Printf(TEXT("Package target is outside the selected Git repository: %s"), *Candidate.Filename);
			return false;
		}
		const FString Revision = InPinnedHead.IsEmpty() ? TEXT("HEAD") : InPinnedHead;
		if (!GitMapPackageSetPrivate::DoesRevisionContainExactPath(InGitBinary, InRepositoryRoot, Revision, Candidate.RepositoryRelativePath, Candidate.bAtRevision, OutError))
		{
			return false;
		}
	}

	TArray<const FCandidate*> Matches;
	for (const FCandidate& Candidate : Candidates)
	{
		if (Candidate.bInWorktree || Candidate.bAtRevision)
		{
			Matches.Add(&Candidate);
		}
	}
	if (Matches.Num() != 1)
	{
		OutError = Matches.IsEmpty()
			? FString::Printf(TEXT("Could not prove a .umap or .uasset target for package '%s' in the worktree or pinned revision."), *InLongPackageName)
			: FString::Printf(TEXT("Package '%s' is ambiguous because both .umap and .uasset targets exist."), *InLongPackageName);
		return false;
	}

	const FCandidate& Match = *Matches[0];
	OutTarget.PackageName = InLongPackageName;
	OutTarget.RepositoryRelativePath = Match.RepositoryRelativePath;
	OutTarget.AbsoluteFilename = Match.Filename;
	OutTarget.bIsMap = Match.bIsMap;
	OutTarget.bExistsInWorktree = Match.bInWorktree;
	OutTarget.bExistsAtPinnedHead = Match.bAtRevision;
	return true;
}

bool GitMapPackageSet::BuildSelectionMutationSetForFiles(const FString& InGitBinary, const FString& InRepositoryRoot,
	const FString& InExpectedPinnedHead, const TArray<FString>& InPrimaryFilenames, const EGitChangedAssetOperationMode InOperationMode,
	FGitChangedAssetMutationSet& OutSet, FString& OutError)
{
	OutSet = FGitChangedAssetMutationSet();
	OutError.Reset();
	if (InPrimaryFilenames.IsEmpty())
	{
		OutError = TEXT("Select at least one primary package file.");
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	if (!FGitChangedAssetsStatus::CaptureSnapshot(InGitBinary, InRepositoryRoot, 0, Snapshot, OutError))
	{
		return false;
	}
	if (!InExpectedPinnedHead.IsEmpty() && !Snapshot.PinnedHead.Equals(InExpectedPinnedHead, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Git HEAD changed before the selected package mutation set was captured.");
		return false;
	}

	TArray<FGitChangedAssetEntry> SelectedEntries;
	TSet<FString> SeenPaths;
	for (FString Filename : InPrimaryFilenames)
	{
		Filename = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Filename);
		const FString Key = GitMapPackageSetPrivate::NormalizePathKey(Filename);
		if (SeenPaths.Contains(Key))
		{
			OutError = FString::Printf(TEXT("The selected package was specified more than once: %s"), *Filename);
			return false;
		}
		SeenPaths.Add(Key);
		const FGitChangedAssetEntry* Entry = Snapshot.Entries.FindByPredicate([&Key](const FGitChangedAssetEntry& Candidate)
		{
			return GitMapPackageSetPrivate::NormalizePathKey(Candidate.AbsoluteFilename) == Key;
		});
		FGitChangedAssetEntry SelectedEntry;
		if (Entry != nullptr)
		{
			SelectedEntry = *Entry;
		}
		else
		{
			if (InOperationMode == EGitChangedAssetOperationMode::DiscardTracked)
			{
				OutError = FString::Printf(TEXT("Discard tracked changes requires a modified tracked primary package in the fixed Git snapshot; clean, added, untracked, ignored, and deleted packages are rejected: %s"), *Filename);
				return false;
			}
			if (!IsGitChangedAssetPrimaryPackagePath(Filename) || !IFileManager::Get().FileExists(*Filename))
			{
				OutError = FString::Printf(TEXT("The selected package is neither a changed primary nor an existing primary package: %s"), *Filename);
				return false;
			}
			FString RepositoryRelativePath;
			if (!GitMapPackageSetPrivate::MakeRepositoryRelativePath(Snapshot.RepositoryRoot, Filename, RepositoryRelativePath))
			{
				OutError = FString::Printf(TEXT("The selected primary package is outside the Git repository: %s"), *Filename);
				return false;
			}
			bool bExistsAtPinnedHead = false;
			if (!GitMapPackageSetPrivate::DoesRevisionContainExactPath(InGitBinary, Snapshot.RepositoryRoot, Snapshot.PinnedHead,
				RepositoryRelativePath, bExistsAtPinnedHead, OutError))
			{
				return false;
			}
			if (InOperationMode != EGitChangedAssetOperationMode::HistoricalRestore && !bExistsAtPinnedHead)
			{
				OutError = FString::Printf(TEXT("The selected clean primary package is not tracked by the fixed Git HEAD: %s"), *Filename);
				return false;
			}
			SelectedEntry.RepositoryRelativePath = MoveTemp(RepositoryRelativePath);
			SelectedEntry.AbsoluteFilename = Filename;
			// This row represents a clean tracked package selected for historical
			// restore. Actual mutation state remains solely in its artifact closure.
			SelectedEntry.State = EGitChangedAssetState::Modified;
			SelectedEntry.IndexStatus = TEXT('.');
			SelectedEntry.WorktreeStatus = TEXT('.');
			SelectedEntry.RecomputeBaseRevertEligibility();
		}
		FString SelectedPackageName;
		if (!FPackageName::TryConvertFilenameToLongPackageName(SelectedEntry.AbsoluteFilename, SelectedPackageName))
		{
			OutError = FString::Printf(TEXT("Could not resolve the selected package name: %s"), *SelectedEntry.AbsoluteFilename);
			return false;
		}
		SelectedEntry.PackageName = MoveTemp(SelectedPackageName);
		const bool bExternalPackage = SelectedEntry.RepositoryRelativePath.Contains(TEXT("/__ExternalActors__/"), ESearchCase::IgnoreCase)
			|| SelectedEntry.RepositoryRelativePath.Contains(TEXT("/__ExternalObjects__/"), ESearchCase::IgnoreCase);
		if (!bExternalPackage)
		{
			SelectedEntry.PackageKind = EGitChangedAssetPackageKind::Regular;
			SelectedEntry.bMetadataResolved = true;
			SelectedEntry.bCanRevert = SelectedEntry.bBaseRevertEligible;
			SelectedEntry.RevertBlockReason.Reset();
		}
		if (InOperationMode == EGitChangedAssetOperationMode::DiscardTracked
			&& ((SelectedEntry.State != EGitChangedAssetState::Modified && SelectedEntry.State != EGitChangedAssetState::Deleted) || SelectedEntry.bHasUntrackedReplacement))
		{
			OutError = FString::Printf(TEXT("Discard tracked changes accepts only a current tracked modified or deleted primary package; added, untracked, renamed, conflicted, and replacement topologies are rejected: %s"), *SelectedEntry.AbsoluteFilename);
			return false;
		}
		SelectedEntries.Add(MoveTemp(SelectedEntry));
	}
	return BuildSelectionMutationSet(Snapshot, SelectedEntries, InOperationMode, OutSet, OutError);
}

bool GitMapPackageSet::EnrichMutationSetForLifecycle(FGitChangedAssetMutationSet& InOutSet, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || InOutSet.SelectedEntries.IsEmpty())
	{
		OutError = TEXT("Standalone Changed Assets lifecycle enrichment requires selected packages on the GameThread.");
		return false;
	}
	for (FGitChangedAssetEntry& Entry : InOutSet.SelectedEntries)
	{
		if (!GitMapPackageSetPrivate::ResolveStandaloneExternalOwner(Entry, OutError))
		{
			return false;
		}
		if (Entry.PackageKind == EGitChangedAssetPackageKind::ExternalActor || Entry.PackageKind == EGitChangedAssetPackageKind::ExternalObject)
		{
			continue;
		}
		if (Entry.PackageName.IsEmpty() && !FPackageName::TryConvertFilenameToLongPackageName(Entry.AbsoluteFilename, Entry.PackageName))
		{
			OutError = FString::Printf(TEXT("Could not resolve selected package metadata for lifecycle: %s"), *Entry.AbsoluteFilename);
			return false;
		}
		Entry.PackageKind = EGitChangedAssetPackageKind::Regular;
		Entry.bMetadataResolved = true;
		Entry.bCanRevert = Entry.bBaseRevertEligible;
		Entry.RevertBlockReason.Reset();
	}
	return true;
}

bool GitMapPackageSet::BuildMapWorldClosure(const FGitChangedAssetEntry& InMapEntry, FGitMapWorldClosure& OutClosure, FString& OutError)
{
	OutClosure = FGitMapWorldClosure();
	OutError.Reset();
	if (!IsInGameThread() || !IsGitChangedAssetMapPath(InMapEntry.RepositoryRelativePath))
	{
		OutError = TEXT("Map world closure requires a .umap entry on the GameThread.");
		return false;
	}
	if (!GitMapPackageSetPrivate::ResolveMapPackageName(InMapEntry, OutClosure.MapPackageName, OutError))
	{
		return false;
	}

	for (const FString& Root : ULevel::GetExternalActorsPaths(OutClosure.MapPackageName))
	{
		GitMapPackageSetPrivate::AddUniquePath(OutClosure.ExternalActorRoots, Root);
	}
	for (const FString& Root : ULevel::GetExternalObjectsPaths(OutClosure.MapPackageName))
	{
		GitMapPackageSetPrivate::AddUniquePath(OutClosure.ExternalObjectRoots, Root);
	}

	if (UPackage* MapPackage = FindPackage(nullptr, *OutClosure.MapPackageName))
	{
		UWorld* const MapWorld = UWorld::FindWorldInPackage(MapPackage);
		if (MapWorld != nullptr)
		{
			if (MapWorld->WorldType == EWorldType::PIE)
			{
				OutError = FString::Printf(TEXT("The selected map is consumed by PIE and cannot enter a Changed Assets lifecycle closure: %s"), *OutClosure.MapPackageName);
				return false;
			}
			if (MapWorld->WorldType != EWorldType::Editor)
			{
				OutError = FString::Printf(TEXT("The selected map is loaded by a non-Editor world and cannot be safely reloaded: %s"), *OutClosure.MapPackageName);
				return false;
			}
			if (UWorldPartition* const WorldPartition = MapWorld->GetWorldPartition())
			{
				if (!WorldPartition->IsInitialized())
				{
					OutError = FString::Printf(TEXT("The selected map WorldPartition is loaded but not initialized, so its ActorDesc container closure is incomplete: %s"), *OutClosure.MapPackageName);
					return false;
				}
				WorldPartition->ForEachActorDescContainerInstance([&OutClosure](UActorDescContainerInstance* Container)
				{
					if (Container != nullptr)
					{
						GitMapPackageSetPrivate::AddUniquePath(OutClosure.ExternalActorRoots, Container->GetExternalActorPath());
						GitMapPackageSetPrivate::AddUniquePath(OutClosure.ExternalObjectRoots, Container->GetExternalObjectPath());
					}
				}, true);
			}
		}
	}

	OutClosure.ExternalActorRoots.Sort();
	OutClosure.ExternalObjectRoots.Sort();
	OutClosure.Signature = OutClosure.MapPackageName + TEXT("\nA:") + FString::Join(OutClosure.ExternalActorRoots, TEXT("\nA:"))
		+ TEXT("\nO:") + FString::Join(OutClosure.ExternalObjectRoots, TEXT("\nO:"));
	return true;
}

bool GitMapPackageSet::BuildPackageRevisionArtifactSet(const FString& InGitBinary, const FString& InRepositoryRoot,
	const FString& InRevision, const FGitChangedPrimaryPackageTarget& InPrimaryTarget, FGitPackageRevisionArtifactSet& OutSet, FString& OutError)
{
	OutSet = FGitPackageRevisionArtifactSet();
	OutError.Reset();
	if (InGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || InRevision.IsEmpty() || InPrimaryTarget.RepositoryRelativePath.IsEmpty()
		|| !IsGitChangedAssetPrimaryPackagePath(InPrimaryTarget.RepositoryRelativePath))
	{
		OutError = TEXT("Historical package validation requires Git, repository, revision, and one exact primary package target.");
		return false;
	}

	bool bPrimaryExists = false;
	if (!GitMapPackageSetPrivate::DoesRevisionContainExactPath(InGitBinary, InRepositoryRoot, InRevision,
		InPrimaryTarget.RepositoryRelativePath, bPrimaryExists, OutError))
	{
		return false;
	}
	if (!bPrimaryExists)
	{
		OutError = FString::Printf(TEXT("The requested revision does not contain the selected primary package: %s"), *InPrimaryTarget.RepositoryRelativePath);
		return false;
	}
	OutSet.Revision = InRevision;
	OutSet.PrimaryTarget = InPrimaryTarget;
	OutSet.RepositoryRelativePaths.Add(InPrimaryTarget.RepositoryRelativePath);
	TArray<FString> Sidecars;
	GitMapPackageSetPrivate::AddPackageSidecarCandidates(InPrimaryTarget.RepositoryRelativePath, Sidecars);
	for (const FString& RelativeSidecar : Sidecars)
	{
		bool bSidecarExists = false;
		if (!GitMapPackageSetPrivate::DoesRevisionContainExactPath(InGitBinary, InRepositoryRoot, InRevision, RelativeSidecar, bSidecarExists, OutError))
		{
			return false;
		}
		if (bSidecarExists)
		{
			OutSet.RepositoryRelativePaths.Add(RelativeSidecar);
		}
	}
	OutSet.RepositoryRelativePaths.Sort();
	OutSet.bComplete = true;
	return true;
}

bool GitMapPackageSet::ValidatePackageRevisionPrimaryForRestore(const FString& InGitBinary, const FString& InRepositoryRoot,
	const FGitPackageRevisionArtifactSet& InRevisionSet, FString& OutError)
{
	OutError.Reset();
	if (!InRevisionSet.bComplete || InRevisionSet.Revision.IsEmpty() || InRevisionSet.PrimaryTarget.RepositoryRelativePath.IsEmpty()
		|| !InRevisionSet.RepositoryRelativePaths.Contains(InRevisionSet.PrimaryTarget.RepositoryRelativePath))
	{
		OutError = TEXT("Historical package restore requires a complete revision artifact set with its primary package.");
		return false;
	}
	return GitMapPackageSetPrivate::ValidateHistoricalPrimaryPackage(InGitBinary, InRepositoryRoot, InRevisionSet.Revision,
		InRevisionSet.PrimaryTarget.RepositoryRelativePath, OutError);
}

bool GitMapPackageSet::MaterializePackageRevisionArtifacts(const FString& InGitBinary, const FString& InRepositoryRoot,
	const FGitPackageRevisionArtifactSet& InRevisionSet, FGitPackageRevisionMaterialization& OutMaterialization, FString& OutError)
{
	OutMaterialization = FGitPackageRevisionMaterialization();
	OutError.Reset();
	if (InGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || !InRevisionSet.bComplete || InRevisionSet.Revision.IsEmpty()
		|| InRevisionSet.RepositoryRelativePaths.IsEmpty() || InRevisionSet.PrimaryTarget.RepositoryRelativePath.IsEmpty())
	{
		OutError = TEXT("Historical package materialization requires a complete revision artifact set.");
		return false;
	}
	if (!InRevisionSet.RepositoryRelativePaths.Contains(InRevisionSet.PrimaryTarget.RepositoryRelativePath))
	{
		OutError = TEXT("Historical package materialization is missing its primary package artifact.");
		return false;
	}

	FGitPackageRevisionMaterialization Candidate;
	Candidate.TemporaryDirectory = FPaths::Combine(FPlatformProcess::UserTempDir(),
		TEXT("git-source-control-package-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*Candidate.TemporaryDirectory, true))
	{
		OutError = FString::Printf(TEXT("Could not create the temporary package materialization directory: %s"), *Candidate.TemporaryDirectory);
		return false;
	}
	bool bPublished = false;
	ON_SCOPE_EXIT
	{
		if (!bPublished)
		{
			IFileManager::Get().DeleteDirectory(*Candidate.TemporaryDirectory, false, true);
		}
	};

	FGitLfsLocalObjectStore LfsObjectStore(InGitBinary, InRepositoryRoot);
	TSet<FString> SeenFilenames;
	for (const FString& RepositoryRelativePath : InRevisionSet.RepositoryRelativePaths)
	{
		const FString Filename = FPaths::GetCleanFilename(RepositoryRelativePath);
		if (Filename.IsEmpty() || Filename != FPaths::GetCleanFilename(Filename) || SeenFilenames.Contains(Filename.ToLower()))
		{
			OutError = FString::Printf(TEXT("Historical package materialization received an unsafe or colliding artifact filename: %s"), *RepositoryRelativePath);
			return false;
		}
		SeenFilenames.Add(Filename.ToLower());
		const FString OutputFilename = FPaths::Combine(Candidate.TemporaryDirectory, Filename);
		if (!GitSourceControlUtils::DumpRevisionBlobToFile(InGitBinary, InRepositoryRoot,
			InRevisionSet.Revision + TEXT(":") + RepositoryRelativePath, OutputFilename, OutError))
		{
			return false;
		}

		FGitLfsPointer Pointer;
		const EGitLfsPointerParseResult PointerResult = ParseGitLfsPointerFile(OutputFilename, Pointer);
		if (PointerResult == EGitLfsPointerParseResult::InvalidPointer)
		{
			OutError = FString::Printf(TEXT("Historical package artifact contains an invalid Git LFS pointer: %s"), *RepositoryRelativePath);
			return false;
		}
		if (PointerResult == EGitLfsPointerParseResult::ValidPointer)
		{
			FString ObjectFilename;
			EGitLfsLocalObjectLookupResult LookupResult = LfsObjectStore.FindObject(Pointer, ObjectFilename, OutError);
			if (LookupResult == EGitLfsLocalObjectLookupResult::Error)
			{
				return false;
			}
			if (LookupResult != EGitLfsLocalObjectLookupResult::Found
				&& !GitSourceControlUtils::FetchLfsContentForRevision(InGitBinary, InRepositoryRoot, InRevisionSet.Revision, RepositoryRelativePath, OutError))
			{
				return false;
			}
			if (LookupResult != EGitLfsLocalObjectLookupResult::Found)
			{
				LfsObjectStore.InvalidateCachedObject(Pointer);
				LookupResult = LfsObjectStore.FindObject(Pointer, ObjectFilename, OutError);
			}
			if (LookupResult != EGitLfsLocalObjectLookupResult::Found
				|| !GitSourceControlUtils::VerifyLocalLfsObject(InGitBinary, InRepositoryRoot, ObjectFilename, Pointer.Oid, Pointer.Size, OutError)
				|| IFileManager::Get().Copy(*OutputFilename, *ObjectFilename, true, true) != COPY_OK)
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Could not materialize the local Git LFS object for historical package artifact: %s"), *RepositoryRelativePath);
				}
				return false;
			}
		}

		Candidate.ArtifactFilenames.Add(OutputFilename);
		Candidate.FilenameByRepositoryRelativePath.Add(RepositoryRelativePath, OutputFilename);
		if (RepositoryRelativePath.Equals(InRevisionSet.PrimaryTarget.RepositoryRelativePath, ESearchCase::CaseSensitive))
		{
			Candidate.PrimaryFilename = OutputFilename;
		}
	}
	if (Candidate.PrimaryFilename.IsEmpty())
	{
		OutError = TEXT("Historical package materialization did not publish a primary package filename.");
		return false;
	}
	bPublished = true;
	OutMaterialization = MoveTemp(Candidate);
	return true;
}
