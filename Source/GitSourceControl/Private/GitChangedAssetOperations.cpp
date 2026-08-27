// Copyright (c) 2026
//
// Changed Assets 的 providerless Revert to HEAD 事务. 只处理单 .uasset.

#include "GitChangedAssetOperations.h"

#include "Algo/AllOf.h"
#include "GitChangedAssetsStatus.h"
#include "GitRepositoryMutationGuard.h"
#include "GitSourceControlFileStatus.h"
#include "GitSourceControlUtils.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

namespace GitChangedAssetOperationsPrivate
{
	struct FFileFingerprint
	{
		bool bExists = false;
		int64 Size = 0;
		FDateTime ModifiedTime;
		FString ContentHash;
		bool bHashValid = true;

		bool operator==(const FFileFingerprint& Other) const
		{
			return bExists == Other.bExists && (!bExists ||
				(bHashValid && Other.bHashValid && Size == Other.Size && ModifiedTime == Other.ModifiedTime && ContentHash == Other.ContentHash));
		}
	};

	struct FFileBackup
	{
		FString Filename;
		FString BackupFilename;
		bool bExisted = false;
	};

	struct FRevertPlan
	{
		TArray<FString> AllFiles;
		TArray<FString> RestoreFromHeadFiles;
		TArray<FString> RemoveFromIndexFiles;
		TArray<FString> DeleteFromWorktreeFiles;
	};

	FString NormalizePathKey(const FString& InFilename)
	{
		FString Result = InFilename;
		FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
		Result.ToLowerInline();
#endif
		return Result;
	}

	void AddUniqueAbsolutePath(TArray<FString>& InOutFiles, const FString& InFilename)
	{
		const FString Key = NormalizePathKey(InFilename);
		if (!InOutFiles.ContainsByPredicate([&Key](const FString& Existing)
		{
			return NormalizePathKey(Existing) == Key;
		}))
		{
			InOutFiles.Add(InFilename);
		}
	}

	bool IsCompleteObjectId(const FString& InCommitId)
	{
		if (InCommitId.Len() != 40 && InCommitId.Len() != 64)
		{
			return false;
		}
		return Algo::AllOf(InCommitId, [](const TCHAR Character)
		{
			return FChar::IsHexDigit(Character);
		});
	}

	bool IsSupportedUassetFilename(const FString& InFilename)
	{
		return !InFilename.IsEmpty() && FPaths::GetExtension(InFilename, false).Equals(TEXT("uasset"), ESearchCase::IgnoreCase);
	}

	bool NormalizeAbsolutePath(const FString& InRepositoryRoot, const FString& InInput, FString& OutFilename, FString& OutRelativePath, FString& OutError)
	{
		OutFilename = FPaths::ConvertRelativePathToFull(InInput);
		FPaths::NormalizeFilename(OutFilename);
		FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		FPaths::NormalizeDirectoryName(RepositoryRoot);
		if (!RepositoryRoot.EndsWith(TEXT("/")))
		{
			RepositoryRoot += TEXT("/");
		}
		if (FPaths::DirectoryExists(OutFilename) || !FPaths::IsUnderDirectory(OutFilename, RepositoryRoot))
		{
			OutError = FString::Printf(TEXT("Changed Assets only accepts explicit files inside the repository: %s"), *OutFilename);
			return false;
		}

		OutRelativePath = OutFilename;
		if (!FPaths::MakePathRelativeTo(OutRelativePath, *RepositoryRoot))
		{
			OutError = FString::Printf(TEXT("Could not make the selected file repository-relative: %s"), *OutFilename);
			return false;
		}
		OutRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (OutRelativePath.StartsWith(TEXT("./")))
		{
			OutRelativePath.RightChopInline(2);
		}
		if (OutRelativePath.IsEmpty() || OutRelativePath.StartsWith(TEXT("/")) || OutRelativePath.Contains(TEXT("../")) || OutRelativePath.Contains(TEXT("..\\")) ||
			OutRelativePath.Contains(TEXT("\n")) || OutRelativePath.Contains(TEXT("\r")))
		{
			OutError = FString::Printf(TEXT("The selected Git path is not a safe literal file path: %s"), *OutFilename);
			return false;
		}
		return true;
	}

	bool ReadHeadCommitId(const FString& InGitBinary, const FString& InRepositoryRoot, FString& OutCommitId, FString& OutError)
	{
		OutCommitId.Reset();
		OutError.Reset();
		FString Output;
		if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), InGitBinary, InRepositoryRoot,
			{ TEXT("--verify"), TEXT("HEAD") }, {}, Output, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Could not resolve the current Git HEAD commit.");
			}
			return false;
		}
		Output.TrimStartAndEndInline();
		if (!IsCompleteObjectId(Output))
		{
			OutError = TEXT("Git did not return a complete HEAD commit id.");
			return false;
		}
		OutCommitId = MoveTemp(Output);
		return true;
	}

	bool VerifyPinnedHead(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InPinnedHead, FString& OutError)
	{
		FString CurrentHead;
		if (!ReadHeadCommitId(InGitBinary, InRepositoryRoot, CurrentHead, OutError))
		{
			return false;
		}
		if (!CurrentHead.Equals(InPinnedHead, ESearchCase::CaseSensitive))
		{
			OutError = TEXT("Git HEAD changed after the Changed Assets snapshot was captured. Refresh and retry.");
			return false;
		}
		return true;
	}

	FFileFingerprint CaptureFingerprint(const FString& InFilename)
	{
		FFileFingerprint Result;
		Result.bExists = IFileManager::Get().FileExists(*InFilename);
		if (!Result.bExists)
		{
			return Result;
		}
		Result.Size = IFileManager::Get().FileSize(*InFilename);
		Result.ModifiedTime = IFileManager::Get().GetTimeStamp(*InFilename);
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *InFilename))
		{
			Result.bHashValid = false;
			return Result;
		}
		FSHA1 Sha;
		Sha.Update(Data.GetData(), Data.Num());
		Sha.Final();
		uint8 Digest[FSHA1::DigestSize];
		Sha.GetHash(Digest);
		Result.ContentHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		return Result;
	}

	bool CreateBackups(const TArray<FString>& InFiles, TArray<FFileBackup>& OutBackups, FString& OutError)
	{
		OutBackups.Reset();
		for (const FString& Filename : InFiles)
		{
			FFileBackup& Backup = OutBackups.AddDefaulted_GetRef();
			Backup.Filename = Filename;
			Backup.bExisted = IFileManager::Get().FileExists(*Filename);
			if (!Backup.bExisted)
			{
				continue;
			}

			Backup.BackupFilename = FPaths::CreateTempFilename(*FPaths::GetPath(Filename), TEXT(".git-changed-assets-backup-"), TEXT(".tmp"));
			if (IFileManager::Get().Copy(*Backup.BackupFilename, *Filename, true, true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("Could not create a safety backup for '%s'. Existing safety backups were preserved."), *Filename);
				return false;
			}
		}
		return true;
	}

	bool RestoreBackups(const TArray<FFileBackup>& InBackups, TArray<FString>& OutFailedBackupLocations)
	{
		bool bSucceeded = true;
		for (const FFileBackup& Backup : InBackups)
		{
			bool bRestored = false;
			if (Backup.bExisted)
			{
				bRestored = IFileManager::Get().Copy(*Backup.Filename, *Backup.BackupFilename, true, true) == COPY_OK;
			}
			else
			{
				bRestored = !IFileManager::Get().FileExists(*Backup.Filename) || IFileManager::Get().Delete(*Backup.Filename, false, true, true);
			}
			if (!bRestored)
			{
				bSucceeded = false;
				OutFailedBackupLocations.Add(Backup.BackupFilename.IsEmpty() ? Backup.Filename : Backup.BackupFilename);
			}
		}
		return bSucceeded;
	}

	void DeleteBackups(const TArray<FFileBackup>& InBackups)
	{
		for (const FFileBackup& Backup : InBackups)
		{
			if (!Backup.BackupFilename.IsEmpty())
			{
				IFileManager::Get().Delete(*Backup.BackupFilename, false, true, true);
			}
		}
	}

	bool SnapshotsEqual(const FGitIndexSnapshot& A, const FGitIndexSnapshot& B)
	{
		return A.RepositoryRoot.Equals(B.RepositoryRoot, ESearchCase::IgnoreCase) && A.Paths.Num() == B.Paths.Num() &&
			A.IndexInfo.Num() == B.IndexInfo.Num() && (A.IndexInfo.Num() == 0 || FMemory::Memcmp(A.IndexInfo.GetData(), B.IndexInfo.GetData(), A.IndexInfo.Num()) == 0);
	}

	const FGitChangedAssetEntry* FindSnapshotEntry(const FGitChangedAssetSnapshot& InSnapshot, const FString& InFilename)
	{
		const FString Key = NormalizePathKey(InFilename);
		for (const FGitChangedAssetEntry& Candidate : InSnapshot.Entries)
		{
			if (NormalizePathKey(Candidate.AbsoluteFilename) == Key)
			{
				return &Candidate;
			}
		}
		return nullptr;
	}

	bool HasSameGitTopology(const FGitChangedAssetEntry& Expected, const FGitChangedAssetEntry& Current)
	{
		return Expected.State == Current.State
			&& Expected.IndexStatus == Current.IndexStatus
			&& Expected.WorktreeStatus == Current.WorktreeStatus
			&& Expected.bHasUntrackedReplacement == Current.bHasUntrackedReplacement
			&& Expected.RepositoryRelativePath.Equals(Current.RepositoryRelativePath, ESearchCase::CaseSensitive)
			&& Expected.RenameFromRepositoryRelativePath.Equals(Current.RenameFromRepositoryRelativePath, ESearchCase::CaseSensitive);
	}

	bool ValidateCurrentStatus(const FString& InGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InExactPaths,
		const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
	{
		TArray<uint8> StatusOutput;
		if (!GitSourceControlUtils::RunPathsStatusPorcelainV2(InGitBinary, InRepositoryRoot, InExactPaths, StatusOutput, OutError))
		{
			return false;
		}
		FGitChangedAssetSnapshot CurrentSnapshot;
		if (!FGitChangedAssetsStatus::ParsePorcelainV2(StatusOutput, InRepositoryRoot, CurrentSnapshot.Entries, OutError))
		{
			return false;
		}
		for (const FGitChangedAssetEntry& Expected : InEntries)
		{
			const FGitChangedAssetEntry* Current = FindSnapshotEntry(CurrentSnapshot, Expected.AbsoluteFilename);
			if (Current == nullptr || !HasSameGitTopology(Expected, *Current))
			{
				OutError = FString::Printf(TEXT("The selected asset's repository-wide Git status changed while Revert was pending: %s"), *Expected.AbsoluteFilename);
				return false;
			}
		}
		return true;
	}

	bool BuildPlan(const FString& InRepositoryRoot, const TArray<FGitChangedAssetEntry>& InEntries, FRevertPlan& OutPlan, FString& OutError)
	{
		OutPlan = FRevertPlan();
		TSet<FString> SeenFiles;
		for (const FGitChangedAssetEntry& Entry : InEntries)
		{
			auto AddPath = [&InRepositoryRoot, &OutPlan, &OutError, &SeenFiles](const FString& Input, const FString& ExpectedRelative, TArray<FString>* Destination) -> bool
			{
				FString Filename;
				FString Relative;
				if (!NormalizeAbsolutePath(InRepositoryRoot, Input, Filename, Relative, OutError))
				{
					return false;
				}
				if (!IsSupportedUassetFilename(Filename))
				{
					OutError = FString::Printf(TEXT("Changed Assets Revert supports only individual .uasset files: %s"), *Filename);
					return false;
				}
				FString NormalizedExpected = ExpectedRelative;
				NormalizedExpected.ReplaceInline(TEXT("\\"), TEXT("/"));
				while (NormalizedExpected.StartsWith(TEXT("./")))
				{
					NormalizedExpected.RightChopInline(2);
				}
				if (!NormalizedExpected.IsEmpty() && !Relative.Equals(NormalizedExpected, ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("Changed Assets path metadata does not match the exact workspace file: %s"), *Filename);
					return false;
				}
				const FString Key = NormalizePathKey(Filename);
				if (SeenFiles.Contains(Key))
				{
					OutError = FString::Printf(TEXT("Selected Changed Assets entries overlap on the same file: %s"), *Filename);
					return false;
				}
				SeenFiles.Add(Key);
				OutPlan.AllFiles.Add(Filename);
				if (Destination != nullptr)
				{
					Destination->Add(Filename);
				}
				return true;
			};

			switch (Entry.State)
			{
			case EGitChangedAssetState::Modified:
			case EGitChangedAssetState::Deleted:
				if (!AddPath(Entry.AbsoluteFilename, Entry.RepositoryRelativePath, &OutPlan.RestoreFromHeadFiles)) return false;
				break;
			case EGitChangedAssetState::Added:
			case EGitChangedAssetState::Untracked:
				if (!AddPath(Entry.AbsoluteFilename, Entry.RepositoryRelativePath, &OutPlan.RemoveFromIndexFiles)) return false;
				OutPlan.DeleteFromWorktreeFiles.Add(OutPlan.RemoveFromIndexFiles.Last());
				break;
			case EGitChangedAssetState::Renamed:
				if (!AddPath(Entry.RenameFromAbsoluteFilename, Entry.RenameFromRepositoryRelativePath, &OutPlan.RestoreFromHeadFiles)) return false;
				if (!AddPath(Entry.AbsoluteFilename, Entry.RepositoryRelativePath, &OutPlan.RemoveFromIndexFiles)) return false;
				OutPlan.DeleteFromWorktreeFiles.Add(OutPlan.RemoveFromIndexFiles.Last());
				break;
			default:
				OutError = FString::Printf(TEXT("Changed Assets entry has an unsupported Revert state: %s"), *Entry.AbsoluteFilename);
				return false;
			}
		}
		return !OutPlan.AllFiles.IsEmpty();
	}

	bool TryParseLfsPointer(const FString& InPointerFilename, FString& OutOid, int64& OutSize, FString& OutError)
	{
		OutOid.Reset();
		OutSize = 0;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *InPointerFilename))
		{
			OutError = TEXT("Could not read the exact HEAD Git blob while checking Git LFS content.");
			return false;
		}
		if (!Text.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")))
		{
			return true;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("oid sha256:")))
			{
				OutOid = Line.Mid(11).TrimStartAndEnd();
			}
			else if (Line.StartsWith(TEXT("size ")))
			{
				const FString SizeText = Line.Mid(5).TrimStartAndEnd();
				if (SizeText.IsEmpty() || !Algo::AllOf(SizeText, [](const TCHAR Character) { return Character >= TEXT('0') && Character <= TEXT('9'); }))
				{
					OutError = TEXT("The HEAD Git LFS pointer has an invalid size.");
					return false;
				}
				OutSize = FCString::Atoi64(*SizeText);
			}
		}
		if (OutOid.IsEmpty())
		{
			OutError = TEXT("The HEAD Git LFS pointer is missing its SHA-256 object id.");
			return false;
		}
		if (OutOid.Len() != 64 || !Algo::AllOf(OutOid, [](const TCHAR Character) { return FChar::IsHexDigit(Character); }))
		{
			OutError = TEXT("The HEAD Git LFS pointer has an invalid SHA-256 object id.");
			return false;
		}
		return true;
	}

	bool ResolveLfsObjectFilename(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InOid, FString& OutObjectFilename, FString& OutError)
	{
		FString Storage;
		FString Output;
		FString ConfigError;
		if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), InGitBinary, InRepositoryRoot,
			{ TEXT("--path"), TEXT("--get"), TEXT("lfs.storage") }, {}, Output, ConfigError, 0, false))
		{
			Storage = Output.TrimStartAndEnd();
		}
		if (Storage.IsEmpty())
		{
			FString GitCommonDirectory;
			if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), InGitBinary, InRepositoryRoot,
				{ TEXT("--git-common-dir") }, {}, GitCommonDirectory, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Could not resolve the local Git common directory for Git LFS.");
				}
				return false;
			}
			Storage = FPaths::Combine(GitCommonDirectory.TrimStartAndEnd(), TEXT("lfs"));
		}
		if (FPaths::IsRelative(Storage))
		{
			Storage = FPaths::ConvertRelativePathToFull(InRepositoryRoot, Storage);
		}
		OutObjectFilename = FPaths::Combine(Storage, TEXT("objects"), InOid.Left(2), InOid.Mid(2, 2), InOid);
		return true;
	}

	bool EnsureHeadLfsObjectsAvailable(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InPinnedHead,
		const TArray<FString>& InRestoreFiles, FString& OutError)
	{
		for (const FString& Filename : InRestoreFiles)
		{
			FString Absolute;
			FString Relative;
			if (!NormalizeAbsolutePath(InRepositoryRoot, Filename, Absolute, Relative, OutError))
			{
				return false;
			}
			const FString PointerFilename = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(), TEXT("git-changed-assets-lfs-"), TEXT(".tmp"));
			ON_SCOPE_EXIT { IFileManager::Get().Delete(*PointerFilename, false, true, true); };
			if (!GitSourceControlUtils::DumpRevisionBlobToFile(InGitBinary, InRepositoryRoot, InPinnedHead + TEXT(":") + Relative, PointerFilename, OutError))
			{
				return false;
			}
			FString Oid;
			int64 Size = 0;
			if (!TryParseLfsPointer(PointerFilename, Oid, Size, OutError))
			{
				return false;
			}
			if (Oid.IsEmpty())
			{
				continue;
			}
			FString ObjectFilename;
			if (!ResolveLfsObjectFilename(InGitBinary, InRepositoryRoot, Oid, ObjectFilename, OutError))
			{
				return false;
			}
			FString VerifyError;
			if (GitSourceControlUtils::VerifyLocalLfsObject(InGitBinary, InRepositoryRoot, ObjectFilename, Oid, Size, VerifyError))
			{
				continue;
			}
			if (!GitSourceControlUtils::FetchLfsContentForRevision(InGitBinary, InRepositoryRoot, InPinnedHead, Relative, OutError))
			{
				return false;
			}
			if (!GitSourceControlUtils::VerifyLocalLfsObject(InGitBinary, InRepositoryRoot, ObjectFilename, Oid, Size, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool RestoreExactPathsFromPinnedHead(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InPinnedHead,
		const TArray<FString>& InFiles, FString& OutError)
	{
		if (InFiles.IsEmpty())
		{
			return true;
		}
		TArray<FString> RelativeFiles;
		RelativeFiles.Reserve(InFiles.Num());
		for (const FString& Filename : InFiles)
		{
			FString Absolute;
			FString Relative;
			if (!NormalizeAbsolutePath(InRepositoryRoot, Filename, Absolute, Relative, OutError))
			{
				return false;
			}
			RelativeFiles.Add(MoveTemp(Relative));
		}
		FString Output;
		// --literal-pathspecs is a global Git option and therefore must precede the
		// restore verb. `--` alone does not disable []/* pathspec magic.
		return GitSourceControlUtils::RunCommandInternalRaw(TEXT("--literal-pathspecs restore"), InGitBinary, InRepositoryRoot,
			{ TEXT("--source=") + InPinnedHead, TEXT("--staged"), TEXT("--worktree") }, RelativeFiles, Output, OutError);
	}

	bool ResetExactIndexPathsToPinnedHead(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InPinnedHead,
		const TArray<FString>& InFiles, FString& OutError)
	{
		if (InFiles.IsEmpty())
		{
			return true;
		}
		TArray<FString> RelativeFiles;
		RelativeFiles.Reserve(InFiles.Num());
		for (const FString& Filename : InFiles)
		{
			FString Absolute;
			FString Relative;
			if (!NormalizeAbsolutePath(InRepositoryRoot, Filename, Absolute, Relative, OutError))
			{
				return false;
			}
			RelativeFiles.Add(MoveTemp(Relative));
		}
		FString Output;
		return GitSourceControlUtils::RunCommandInternalRaw(TEXT("--literal-pathspecs reset"), InGitBinary, InRepositoryRoot,
			{ TEXT("-q"), InPinnedHead }, RelativeFiles, Output, OutError);
	}

	bool DeleteExactFiles(const TArray<FString>& InFiles, FString& OutError)
	{
		for (const FString& Filename : InFiles)
		{
			if (IFileManager::Get().FileExists(*Filename) && !IFileManager::Get().Delete(*Filename, false, true, true))
			{
				OutError = FString::Printf(TEXT("Could not delete the exact Added or Untracked asset file '%s'."), *Filename);
				return false;
			}
		}
		return true;
	}

	UPackage* FindEntryPackage(const FGitChangedAssetEntry& InEntry)
	{
		FString PackageName = InEntry.PackageName;
		if (PackageName.IsEmpty())
		{
			FPackageName::TryConvertFilenameToLongPackageName(InEntry.AbsoluteFilename, PackageName);
		}
		return PackageName.IsEmpty() ? nullptr : FindPackage(nullptr, *PackageName);
	}

	bool IsExternalEntry(const FGitChangedAssetEntry& InEntry)
	{
		return InEntry.PackageKind == EGitChangedAssetPackageKind::ExternalActor || InEntry.PackageKind == EGitChangedAssetPackageKind::ExternalObject;
	}

	bool IsExternalFilename(const FString& InFilename)
	{
		return InFilename.Contains(TEXT("__ExternalActors__"), ESearchCase::IgnoreCase)
			|| InFilename.Contains(TEXT("__ExternalObjects__"), ESearchCase::IgnoreCase);
	}

	UPackage* FindPackageForFilename(const FString& InFilename)
	{
		FString PackageName;
		return FPackageName::TryConvertFilenameToLongPackageName(InFilename, PackageName) ? FindPackage(nullptr, *PackageName) : nullptr;
	}

	struct FEditorLifecyclePlan
	{
		TSet<UPackage*> PackagesToResetLoaders;
		TSet<UPackage*> PackagesToReload;
		TSet<UPackage*> SelectedPackages;
		TSet<UPackage*> OwnerWorldPackages;
		TArray<FString> SelectedDirtyPackageNames;
		TArray<FString> OwnerMapsToReload;
	};

	void AddOwnerWorldPackage(UPackage* InOwnerPackage, FEditorLifecyclePlan& InOutPlan)
	{
		if (InOwnerPackage == nullptr || InOutPlan.OwnerWorldPackages.Contains(InOwnerPackage))
		{
			return;
		}
		InOutPlan.OwnerWorldPackages.Add(InOwnerPackage);
		InOutPlan.PackagesToResetLoaders.Add(InOwnerPackage);
		InOutPlan.PackagesToReload.Add(InOwnerPackage);
		InOutPlan.OwnerMapsToReload.Add(InOwnerPackage->GetName());
	}

	void AddOwnerWorldFromExternalPackage(UPackage* InExternalPackage, FEditorLifecyclePlan& InOutPlan)
	{
		if (InExternalPackage == nullptr)
		{
			return;
		}
		if (UObject* Asset = InExternalPackage->FindAssetInPackage(); Asset != nullptr && Asset->IsPackageExternal())
		{
			if (UWorld* World = Asset->GetWorld())
			{
				AddOwnerWorldPackage(World->GetPackage(), InOutPlan);
			}
		}
	}

	bool AddLoadedWorldOwnersForExternalFilename(const FString& InFilename, FEditorLifecyclePlan& InOutPlan, FString& OutError)
	{
		FString ExternalPackageName;
		if (!FPackageName::TryConvertFilenameToLongPackageName(InFilename, ExternalPackageName))
		{
			OutError = FString::Printf(TEXT("Could not resolve the renamed external package name: %s"), *InFilename);
			return false;
		}

		TSet<UPackage*> Matches;
		for (TObjectIterator<ULevel> It; It; ++It)
		{
			ULevel* Level = *It;
			if (Level == nullptr || Level->GetWorld() == nullptr || Level->GetWorld()->GetPackage() == nullptr)
			{
				continue;
			}
			if (Level->IsUsingExternalActors())
			{
				for (const FString& ActorsPath : ULevel::GetExternalActorsPaths(Level->GetPackage()->GetName()))
				{
					if (ExternalPackageName.StartsWith(ActorsPath + TEXT("/"), ESearchCase::IgnoreCase))
					{
						Matches.Add(Level->GetWorld()->GetPackage());
					}
				}
			}
			if (Level->IsUsingExternalObjects())
			{
				for (const FString& ObjectsPath : ULevel::GetExternalObjectsPaths(Level->GetPackage()->GetName()))
				{
					if (ExternalPackageName.StartsWith(ObjectsPath + TEXT("/"), ESearchCase::IgnoreCase))
					{
						Matches.Add(Level->GetWorld()->GetPackage());
					}
				}
			}
		}
		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("The renamed OFPA source matches multiple loaded owner maps: %s"), *InFilename);
			return false;
		}
		for (UPackage* OwnerPackage : Matches)
		{
			AddOwnerWorldPackage(OwnerPackage, InOutPlan);
		}
		return true;
	}

	bool BuildEditorLifecyclePlan(const TArray<FGitChangedAssetEntry>& InEntries, FEditorLifecyclePlan& OutPlan, FString& OutError)
	{
		OutPlan = FEditorLifecyclePlan();
		for (const FGitChangedAssetEntry& Entry : InEntries)
		{
			UPackage* DirectPackage = FindEntryPackage(Entry);
			if (DirectPackage != nullptr)
			{
				OutPlan.SelectedPackages.Add(DirectPackage);
				OutPlan.PackagesToResetLoaders.Add(DirectPackage);
			}
			if (IsExternalEntry(Entry))
			{
				UPackage* OwnerPackage = FindPackage(nullptr, *Entry.OwnerLevel);
				if (DirectPackage != nullptr && OwnerPackage == nullptr)
				{
					OutError = FString::Printf(TEXT("The loaded OFPA package's resolved owner map is not loaded, so its reload closure is unsafe: %s"), *Entry.AbsoluteFilename);
					return false;
				}
				AddOwnerWorldPackage(OwnerPackage, OutPlan);
			}
			else if (DirectPackage != nullptr)
			{
				OutPlan.PackagesToReload.Add(DirectPackage);
			}

			if (!Entry.IsRename())
			{
				continue;
			}
			UPackage* RenameSourcePackage = FindPackageForFilename(Entry.RenameFromAbsoluteFilename);
			if (RenameSourcePackage != nullptr)
			{
				OutPlan.SelectedPackages.Add(RenameSourcePackage);
				OutPlan.PackagesToResetLoaders.Add(RenameSourcePackage);
				AddOwnerWorldFromExternalPackage(RenameSourcePackage, OutPlan);
			}
			if (IsExternalFilename(Entry.RenameFromAbsoluteFilename) && !AddLoadedWorldOwnersForExternalFilename(Entry.RenameFromAbsoluteFilename, OutPlan, OutError))
			{
				return false;
			}
		}

		for (UPackage* OwnerPackage : OutPlan.OwnerWorldPackages)
		{
			if (OwnerPackage->IsDirty())
			{
				OutError = FString::Printf(TEXT("The OFPA owner map has unsaved Editor changes. Save or discard it before Changed Assets Revert: %s"), *OwnerPackage->GetName());
				return false;
			}
		}
		for (UPackage* SelectedPackage : OutPlan.SelectedPackages)
		{
			if (SelectedPackage != nullptr && SelectedPackage->IsDirty())
			{
				OutPlan.SelectedDirtyPackageNames.Add(SelectedPackage->GetName());
			}
		}
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Candidate = *It;
			if (Candidate == nullptr || !Candidate->IsDirty() || OutPlan.SelectedPackages.Contains(Candidate))
			{
				continue;
			}
			UObject* Asset = Candidate->FindAssetInPackage();
			if (Asset == nullptr || !Asset->IsPackageExternal())
			{
				continue;
			}
			UWorld* World = Asset->GetWorld();
			if (World != nullptr && OutPlan.OwnerWorldPackages.Contains(World->GetPackage()))
			{
				OutError = FString::Printf(TEXT("A non-selected external package in an owner map reload closure has unsaved Editor changes: %s"), *Candidate->GetName());
				return false;
			}
		}
		OutPlan.SelectedDirtyPackageNames.Sort();
		OutPlan.OwnerMapsToReload.Sort();
		return true;
	}

	FString MakeLifecycleClosureSignature(const FEditorLifecyclePlan& InPlan)
	{
		TArray<FString> Tokens;
		auto AppendPackages = [&Tokens](const TCHAR* Prefix, const TSet<UPackage*>& Packages)
		{
			for (UPackage* Package : Packages)
			{
				if (Package != nullptr)
				{
					Tokens.Add(FString::Printf(TEXT("%s:%s"), Prefix, *Package->GetName()));
				}
			}
		};
		AppendPackages(TEXT("Reset"), InPlan.PackagesToResetLoaders);
		AppendPackages(TEXT("Reload"), InPlan.PackagesToReload);
		for (const FString& PackageName : InPlan.SelectedDirtyPackageNames)
		{
			Tokens.Add(TEXT("SelectedDirty:") + PackageName);
		}
		for (const FString& PackageName : InPlan.OwnerMapsToReload)
		{
			Tokens.Add(TEXT("Owner:") + PackageName);
		}
		Tokens.Sort();
		return FString::Join(Tokens, TEXT("\n"));
	}
}

namespace GitChangedAssetOperations
{
	bool FGitChangedAssetOperations::ValidateEntries(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
	{
		OutError.Reset();
		if (InEntries.IsEmpty())
		{
			OutError = TEXT("Select at least one Changed Asset to revert.");
			return false;
		}
		for (const FGitChangedAssetEntry& Entry : InEntries)
		{
			if (!Entry.bBaseRevertEligible || !Entry.bCanRevert || Entry.IsConflicted())
			{
				OutError = Entry.RevertBlockReason.IsEmpty()
					? FString::Printf(TEXT("This Changed Asset cannot be reverted: %s"), *Entry.AbsoluteFilename)
					: Entry.RevertBlockReason;
				return false;
			}
			if (!GitChangedAssetOperationsPrivate::IsSupportedUassetFilename(Entry.AbsoluteFilename) || !IsGitChangedAssetUassetPath(Entry.RepositoryRelativePath))
			{
				OutError = FString::Printf(TEXT("Changed Assets Revert supports only individual .uasset files: %s"), *Entry.AbsoluteFilename);
				return false;
			}
			if (Entry.IsRename() && (!GitChangedAssetOperationsPrivate::IsSupportedUassetFilename(Entry.RenameFromAbsoluteFilename) || !IsGitChangedAssetUassetPath(Entry.RenameFromRepositoryRelativePath)))
			{
				OutError = FString::Printf(TEXT("The renamed Changed Asset does not have a valid .uasset source: %s"), *Entry.AbsoluteFilename);
				return false;
			}
			if (GitChangedAssetOperationsPrivate::IsExternalEntry(Entry) && (!Entry.bOwnerLevelResolved || Entry.OwnerLevel.IsEmpty()))
			{
				OutError = FString::Printf(TEXT("The OFPA owner level is unresolved, so this asset cannot be safely reverted: %s"), *Entry.AbsoluteFilename);
				return false;
			}
		}
		return true;
	}

	FGitChangedAssetOperations::FGitChangedAssetOperations(FString InGitBinary, FString InRepositoryRoot)
		: GitBinary(MoveTemp(InGitBinary))
		, RepositoryRoot(FPaths::ConvertRelativePathToFull(InRepositoryRoot))
	{
		FPaths::NormalizeDirectoryName(RepositoryRoot);
	}

	bool FGitChangedAssetOperations::RevertToHead(const FString& InPinnedHead, const TArray<FGitChangedAssetEntry>& InEntries,
		const FGitChangedAssetRevertCallbacks& InCallbacks, FGitChangedAssetRevertResult& OutResult) const
	{
		OutResult = FGitChangedAssetRevertResult();
		if (GitBinary.IsEmpty() || RepositoryRoot.IsEmpty() || !GitChangedAssetOperationsPrivate::IsCompleteObjectId(InPinnedHead))
		{
			OutResult.AddError(TEXT("Changed Assets Revert requires a Git binary, repository root, and complete pinned HEAD commit id."));
			return false;
		}

		GitSourceControlRepositoryMutation::FGitRepositoryMutationGuard TransactionGuard(RepositoryRoot);
		if (!TransactionGuard.Acquire([&InCallbacks]() { return InCallbacks.IsCancellationRequested && InCallbacks.IsCancellationRequested(); }))
		{
			OutResult.bCancelled = InCallbacks.IsCancellationRequested && InCallbacks.IsCancellationRequested();
			if (!OutResult.bCancelled)
			{
				OutResult.AddError(TEXT("Changed Assets Revert could not acquire the repository mutation transaction guard."));
			}
			return false;
		}
		FString Error;
		if (!ValidateEntries(InEntries, Error))
		{
			OutResult.AddError(Error);
			return false;
		}

		GitChangedAssetOperationsPrivate::FRevertPlan Plan;
		if (!GitChangedAssetOperationsPrivate::BuildPlan(RepositoryRoot, InEntries, Plan, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		if (!GitChangedAssetOperationsPrivate::VerifyPinnedHead(GitBinary, RepositoryRoot, InPinnedHead, Error) ||
			!GitChangedAssetOperationsPrivate::ValidateCurrentStatus(GitBinary, RepositoryRoot, Plan.AllFiles, InEntries, Error))
		{
			OutResult.AddError(Error);
			return false;
		}

		TMap<FString, GitChangedAssetOperationsPrivate::FFileFingerprint> Fingerprints;
		for (const FString& Filename : Plan.AllFiles)
		{
			const GitChangedAssetOperationsPrivate::FFileFingerprint Fingerprint = GitChangedAssetOperationsPrivate::CaptureFingerprint(Filename);
			if (!Fingerprint.bHashValid)
			{
				OutResult.AddError(FString::Printf(TEXT("Could not fingerprint '%s' before Changed Assets Revert."), *Filename));
				return false;
			}
			Fingerprints.Add(GitChangedAssetOperationsPrivate::NormalizePathKey(Filename), Fingerprint);
		}
		FGitIndexSnapshot IndexSnapshot;
		if (!GitSourceControlUtils::CaptureIndexEntriesForPaths(GitBinary, RepositoryRoot, Plan.AllFiles, IndexSnapshot, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		if (InCallbacks.Confirm && !InCallbacks.Confirm(InEntries, Error))
		{
			OutResult.bCancelled = Error.IsEmpty();
			OutResult.AddError(Error);
			return false;
		}

		// Only an explicitly confirmed Revert may issue targeted LFS downloads. Do this
		// before unlinking Editor packages so a network failure leaves the Editor intact.
		if (!GitChangedAssetOperationsPrivate::EnsureHeadLfsObjectsAvailable(GitBinary, RepositoryRoot, InPinnedHead, Plan.RestoreFromHeadFiles, Error))
		{
			OutResult.AddError(Error);
			return false;
		}

		bool bPrepared = false;
		auto FinalizePreparedEditor = [&](const EGitChangedAssetMutationOutcome Outcome)
		{
			if (!bPrepared || !InCallbacks.FinalizeEditor)
			{
				return;
			}
			FString FinalizeError;
			if (!InCallbacks.FinalizeEditor(InEntries, Plan.AllFiles, Outcome, FinalizeError))
			{
				OutResult.bReloadSucceeded = false;
				OutResult.AddError(FinalizeError.IsEmpty() ? TEXT("The Editor package reload did not complete." ) : FinalizeError);
			}
		};

		if (InCallbacks.PrepareForMutation && !InCallbacks.PrepareForMutation(InEntries, Error))
		{
			OutResult.AddError(Error.IsEmpty() ? TEXT("The Editor could not prepare the selected packages for Revert.") : Error);
			return false;
		}
		bPrepared = true;

#if WITH_DEV_AUTOMATION_TESTS
		if (InCallbacks.BeforeCommitPointForTesting)
		{
			InCallbacks.BeforeCommitPointForTesting();
		}
#endif
		if (!GitChangedAssetOperationsPrivate::VerifyPinnedHead(GitBinary, RepositoryRoot, InPinnedHead, Error))
		{
			OutResult.AddError(Error);
			FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
			return false;
		}
		for (const FString& Filename : Plan.AllFiles)
		{
			const GitChangedAssetOperationsPrivate::FFileFingerprint* Expected = Fingerprints.Find(GitChangedAssetOperationsPrivate::NormalizePathKey(Filename));
			if (Expected == nullptr || !(GitChangedAssetOperationsPrivate::CaptureFingerprint(Filename) == *Expected))
			{
				OutResult.AddError(FString::Printf(TEXT("The selected file changed while Revert was pending: %s"), *Filename));
				FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
				return false;
			}
		}
		FGitIndexSnapshot CurrentIndexSnapshot;
		if (!GitSourceControlUtils::CaptureIndexEntriesForPaths(GitBinary, RepositoryRoot, Plan.AllFiles, CurrentIndexSnapshot, Error) ||
			!GitChangedAssetOperationsPrivate::SnapshotsEqual(IndexSnapshot, CurrentIndexSnapshot))
		{
			OutResult.AddError(Error.IsEmpty() ? TEXT("The Git index changed while Revert was pending. Refresh and retry.") : Error);
			FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
			return false;
		}
		if (!GitChangedAssetOperationsPrivate::ValidateCurrentStatus(GitBinary, RepositoryRoot, Plan.AllFiles, InEntries, Error))
		{
			OutResult.AddError(Error);
			FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
			return false;
		}

		TArray<GitChangedAssetOperationsPrivate::FFileBackup> Backups;
		if (!GitChangedAssetOperationsPrivate::CreateBackups(Plan.AllFiles, Backups, Error))
		{
			OutResult.AddError(Error);
			FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
			return false;
		}
		if (InCallbacks.BeginMutation && !InCallbacks.BeginMutation(InEntries, Error))
		{
			OutResult.AddError(Error.IsEmpty() ? TEXT("The Editor could not enter the Changed Assets mutation commit point.") : Error);
			GitChangedAssetOperationsPrivate::DeleteBackups(Backups);
			FinalizePreparedEditor(EGitChangedAssetMutationOutcome::NeverMutated);
			return false;
		}

		auto RollBack = [&](const FString& MutationError)
		{
			TArray<FString> FailedBackups;
			const bool bWorktreeRestored = GitChangedAssetOperationsPrivate::RestoreBackups(Backups, FailedBackups);
			FString RestoreIndexError;
			const bool bIndexRestored = GitSourceControlUtils::RestoreIndexEntries(GitBinary, RepositoryRoot, IndexSnapshot, RestoreIndexError);
			OutResult.AddError(MutationError);
			if (!bIndexRestored)
			{
				OutResult.AddError(RestoreIndexError.IsEmpty() ? TEXT("Git index rollback failed.") : RestoreIndexError);
			}
			if (bWorktreeRestored && bIndexRestored)
			{
				GitChangedAssetOperationsPrivate::DeleteBackups(Backups);
			}
			else
			{
				TArray<FString> PreservedBackups;
				for (const GitChangedAssetOperationsPrivate::FFileBackup& Backup : Backups)
				{
					if (!Backup.BackupFilename.IsEmpty())
					{
						PreservedBackups.Add(Backup.BackupFilename);
					}
				}
				OutResult.AddError(FString::Printf(TEXT("Rollback recovery was incomplete. Safety backups were preserved at:\n%s"), *FString::Join(PreservedBackups, TEXT("\n"))));
			}
			FinalizePreparedEditor(bWorktreeRestored && bIndexRestored
				? EGitChangedAssetMutationOutcome::RolledBack
				: EGitChangedAssetMutationOutcome::RollbackFailed);
		};

		if (!GitChangedAssetOperationsPrivate::RestoreExactPathsFromPinnedHead(GitBinary, RepositoryRoot, InPinnedHead, Plan.RestoreFromHeadFiles, Error))
		{
			RollBack(Error.IsEmpty() ? TEXT("Could not restore the selected tracked assets from the pinned HEAD.") : Error);
			return false;
		}
		if (!GitChangedAssetOperationsPrivate::ResetExactIndexPathsToPinnedHead(GitBinary, RepositoryRoot, InPinnedHead, Plan.RemoveFromIndexFiles, Error))
		{
			RollBack(Error.IsEmpty() ? TEXT("Could not reset the exact Added or Renamed Git index paths to the pinned HEAD.") : Error);
			return false;
		}

		bool bAllowFilesystemMutation = true;
#if WITH_DEV_AUTOMATION_TESTS
		if (InCallbacks.AllowFilesystemMutationForTesting)
		{
			bAllowFilesystemMutation = InCallbacks.AllowFilesystemMutationForTesting();
		}
#endif
		if (!bAllowFilesystemMutation || !GitChangedAssetOperationsPrivate::DeleteExactFiles(Plan.DeleteFromWorktreeFiles, Error))
		{
			RollBack(bAllowFilesystemMutation
				? (Error.IsEmpty() ? TEXT("Could not delete the exact Added or Untracked asset files.") : Error)
				: TEXT("Filesystem mutation was rejected by the Changed Assets rollback test seam."));
			return false;
		}

		GitChangedAssetOperationsPrivate::DeleteBackups(Backups);
		OutResult.bSucceeded = true;
		OutResult.AffectedFiles = Plan.AllFiles;
		FinalizePreparedEditor(EGitChangedAssetMutationOutcome::Succeeded);
		return true;
	}

	bool FGitChangedAssetRevertLifecycle::BuildPreview(const TArray<FGitChangedAssetEntry>& InEntries, FGitChangedAssetRevertPreview& OutPreview, FString& OutError)
	{
		OutPreview = FGitChangedAssetRevertPreview();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("Changed Assets lifecycle preview must run on the GameThread.");
			return false;
		}
		if (!FGitChangedAssetOperations::ValidateEntries(InEntries, OutError))
		{
			return false;
		}
		GitChangedAssetOperationsPrivate::FEditorLifecyclePlan Plan;
		if (!GitChangedAssetOperationsPrivate::BuildEditorLifecyclePlan(InEntries, Plan, OutError))
		{
			return false;
		}
		OutPreview.ClosureSignature = GitChangedAssetOperationsPrivate::MakeLifecycleClosureSignature(Plan);
		OutPreview.SelectedDirtyPackageNames = MoveTemp(Plan.SelectedDirtyPackageNames);
		OutPreview.OwnerMapsToReload = MoveTemp(Plan.OwnerMapsToReload);
		return true;
	}

	bool FGitChangedAssetRevertLifecycle::RecordConfirmedClosure(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
	{
		FGitChangedAssetRevertPreview Preview;
		if (!BuildPreview(InEntries, Preview, OutError))
		{
			return false;
		}
		ConfirmedClosureSignature = MoveTemp(Preview.ClosureSignature);
		bClosureConfirmed = true;
		return true;
	}

	bool FGitChangedAssetRevertLifecycle::Prepare(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
	{
		OutError.Reset();
		PackagesToResetLoaders.Reset();
		PackagesToReload.Reset();
		bPrepared = false;
		bLoadersReset = false;
		if (!IsInGameThread())
		{
			OutError = TEXT("Changed Assets package preparation must run on the GameThread.");
			return false;
		}
		if (!FGitChangedAssetOperations::ValidateEntries(InEntries, OutError))
		{
			return false;
		}
		GitChangedAssetOperationsPrivate::FEditorLifecyclePlan Plan;
		if (!GitChangedAssetOperationsPrivate::BuildEditorLifecyclePlan(InEntries, Plan, OutError))
		{
			return false;
		}
		if (!bClosureConfirmed || !ConfirmedClosureSignature.Equals(GitChangedAssetOperationsPrivate::MakeLifecycleClosureSignature(Plan), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("The loaded package, owner-map, or dirty-package closure changed after confirmation. Revert was not started; review and confirm again.");
			return false;
		}
		for (UPackage* Package : Plan.PackagesToResetLoaders)
		{
			PackagesToResetLoaders.Add(Package);
		}
		for (UPackage* Package : Plan.PackagesToReload)
		{
			PackagesToReload.Add(Package);
		}
		bPrepared = true;
		return true;
	}

	bool FGitChangedAssetRevertLifecycle::BeginMutation(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
	{
		static_cast<void>(InEntries);
		OutError.Reset();
		if (!IsInGameThread() || !bPrepared)
		{
			OutError = TEXT("Changed Assets lifecycle was not prepared before entering its mutation commit point.");
			return false;
		}
		GitChangedAssetOperationsPrivate::FEditorLifecyclePlan CurrentPlan;
		if (!GitChangedAssetOperationsPrivate::BuildEditorLifecyclePlan(InEntries, CurrentPlan, OutError))
		{
			return false;
		}
		if (!bClosureConfirmed || !ConfirmedClosureSignature.Equals(GitChangedAssetOperationsPrivate::MakeLifecycleClosureSignature(CurrentPlan), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("The Editor lifecycle closure changed after confirmation. Revert was not started; review and confirm again.");
			return false;
		}
		TArray<UObject*> ObjectsToReset;
		TArray<int32> PendingPackageRequestIds;
		for (const TWeakObjectPtr<UPackage>& WeakPackage : PackagesToResetLoaders)
		{
			UPackage* Package = WeakPackage.Get();
			if (Package == nullptr)
			{
				continue;
			}
			ObjectsToReset.Add(Package);
			if (!Package->IsFullyLoaded())
			{
				PendingPackageRequestIds.Add(LoadPackageAsync(Package->GetName()));
			}
		}
		if (!PendingPackageRequestIds.IsEmpty())
		{
			FlushAsyncLoading(PendingPackageRequestIds);
		}
		if (!ObjectsToReset.IsEmpty())
		{
			ResetLoaders(ObjectsToReset);
		}
		bLoadersReset = true;
		return true;
	}

	bool FGitChangedAssetRevertLifecycle::Finish(const TArray<FGitChangedAssetEntry>& InEntries, const TArray<FString>& InAffectedFiles,
		const EGitChangedAssetMutationOutcome InOutcome, FString& OutError)
	{
		static_cast<void>(InEntries);
		static_cast<void>(InAffectedFiles);
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("Changed Assets package finalization must run on the GameThread.");
			return false;
		}
		if (!bPrepared)
		{
			return true;
		}
		ON_SCOPE_EXIT
		{
			PackagesToResetLoaders.Reset();
			PackagesToReload.Reset();
			ConfirmedClosureSignature.Reset();
			bClosureConfirmed = false;
			bPrepared = false;
			bLoadersReset = false;
		};
		if (InOutcome != EGitChangedAssetMutationOutcome::Succeeded)
		{
			if (!bLoadersReset)
			{
				return true;
			}
			for (const TWeakObjectPtr<UPackage>& WeakPackage : PackagesToResetLoaders)
			{
				UPackage* Package = WeakPackage.Get();
				if (Package != nullptr && Package->GetLinker() == nullptr && LoadPackage(nullptr, *Package->GetName(), LOAD_NoWarn) != Package)
				{
					OutError = FString::Printf(TEXT("Could not restore the prepared package loader without reloading Editor state: %s"), *Package->GetName());
					return false;
				}
			}
			return true;
		}

		TArray<UPackage*> ExistingPackages;
		TArray<UPackage*> MissingPackages;
		for (const TWeakObjectPtr<UPackage>& WeakPackage : PackagesToReload)
		{
			UPackage* Package = WeakPackage.Get();
			if (Package == nullptr)
			{
				continue;
			}
			const FString Extension = Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
			if (!FPaths::FileExists(PackageFilename))
			{
				MissingPackages.Add(Package);
			}
			else
			{
				ExistingPackages.Add(Package);
			}
		}

		if (!MissingPackages.IsEmpty())
		{
			FText UnloadError;
			if (!UPackageTools::UnloadPackages(MissingPackages, UnloadError, true))
			{
				OutError = UnloadError.IsEmpty() ? TEXT("The deleted Changed Asset packages could not be unloaded.") : UnloadError.ToString();
				return false;
			}
		}

		TArray<UPackage*> WorldPackages;
		TArray<UPackage*> NonWorldPackages;
		for (UPackage* Package : ExistingPackages)
		{
			if (UWorld::FindWorldInPackage(Package) != nullptr)
			{
				WorldPackages.Add(Package);
			}
			else
			{
				NonWorldPackages.Add(Package);
			}
		}
		auto ReloadGroup = [&OutError](const TArray<UPackage*>& Packages) -> bool
		{
			if (Packages.IsEmpty())
			{
				return true;
			}
			FText ReloadError;
			UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumePositive);
			if (!ReloadError.IsEmpty())
			{
				OutError = ReloadError.ToString();
				return false;
			}
			return true;
		};
		return ReloadGroup(NonWorldPackages) && ReloadGroup(WorldPackages);
	}
}
