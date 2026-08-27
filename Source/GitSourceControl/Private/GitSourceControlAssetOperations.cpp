// Copyright (c) 2026
//
// Distributed under the MIT License (MIT).

#include "GitSourceControlAssetOperations.h"

#include "Algo/AllOf.h"
#include "GitSourceControlFileStatus.h"
#include "GitRepositoryMutationGuard.h"
#include "GitSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/Package.h"

namespace GitSourceControlAssetOperationsPrivate
{
	struct FFileBackup
	{
		FString Filename;
		FString BackupFilename;
		bool bExisted = false;
	};

	void DeleteBackups(const TArray<FFileBackup>& Backups);

	FString NormalizeKey(const FString& Filename)
	{
		FString Result = Filename;
		FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
		Result.ToLowerInline();
#endif
		return Result;
	}

	bool SamePath(const FString& A, const FString& B)
	{
		return NormalizeKey(A).Equals(NormalizeKey(B), ESearchCase::CaseSensitive);
	}

	FGitSourceControlFileStatus* FindState(TMap<FString, FGitSourceControlFileStatus>& States, const FString& Filename)
	{
		if (FGitSourceControlFileStatus* State = States.Find(Filename))
		{
			return State;
		}
		for (TPair<FString, FGitSourceControlFileStatus>& Pair : States)
		{
			if (SamePath(Pair.Key, Filename))
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}

	bool CreateBackups(const TArray<FString>& Files, TArray<FFileBackup>& OutBackups, FString& OutError)
	{
		for (const FString& Filename : Files)
		{
			FFileBackup& Backup = OutBackups.AddDefaulted_GetRef();
			Backup.Filename = Filename;
			Backup.bExisted = IFileManager::Get().FileExists(*Filename);
			if (!Backup.bExisted)
			{
				continue;
			}

			Backup.BackupFilename = FPaths::CreateTempFilename(*FPaths::GetPath(Filename), TEXT(".git-source-control-txn-backup-"), TEXT(".tmp"));
			if (IFileManager::Get().Copy(*Backup.BackupFilename, *Filename, true, true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("Could not create a safety backup for '%s'. Existing safety backups were preserved."), *Filename);
				return false;
			}
		}
		return true;
	}

	bool RestoreBackups(const TArray<FFileBackup>& Backups, TArray<FString>& OutFailedBackups)
	{
		bool bSucceeded = true;
		for (const FFileBackup& Backup : Backups)
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
				OutFailedBackups.Add(Backup.BackupFilename.IsEmpty() ? Backup.Filename : Backup.BackupFilename);
			}
		}
		return bSucceeded;
	}

	void DeleteBackups(const TArray<FFileBackup>& Backups)
	{
		for (const FFileBackup& Backup : Backups)
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

	bool ReadHeadCommitId(const FString& GitBinary, const FString& RepositoryRoot, FString& OutCommitId, FString& OutError)
	{
		OutCommitId.Reset();
		OutError.Reset();
		FString StandardOutput;
		if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), GitBinary, RepositoryRoot, { TEXT("--verify"), TEXT("HEAD") }, {}, StandardOutput, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Could not resolve the current Git HEAD commit.");
			}
			return false;
		}
		StandardOutput.TrimStartAndEndInline();
		if (StandardOutput.IsEmpty())
		{
			OutError = TEXT("Git did not return the current HEAD commit.");
			return false;
		}
		OutCommitId = MoveTemp(StandardOutput);
		return true;
	}

	bool ResolveFullCommitId(const FString& GitBinary, const FString& RepositoryRoot, const FString& InCommitId, FString& OutCommitId, FString& OutError)
	{
		OutCommitId.Reset();
		OutError.Reset();
		FString StandardOutput;
		if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), GitBinary, RepositoryRoot, { TEXT("--verify"), InCommitId + TEXT("^{commit}") }, {}, StandardOutput, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The requested historical commit is not available locally.");
			}
			return false;
		}
		StandardOutput.TrimStartAndEndInline();
		const bool bCompleteObjectId = (InCommitId.Len() == 40 || InCommitId.Len() == 64)
			&& Algo::AllOf(InCommitId, [](const TCHAR Character)
			{
				return FChar::IsHexDigit(Character);
			});
		if (!bCompleteObjectId || StandardOutput.IsEmpty() || !StandardOutput.Equals(InCommitId, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Historical restore requires a complete immutable commit id.");
			return false;
		}
		OutCommitId = MoveTemp(StandardOutput);
		return true;
	}

	enum class ELfsPointerParseResult : uint8
	{
		NotPointer,
		ValidPointer,
		InvalidPointer,
	};

	bool ParseLfsObjectSize(const FString& Value, int64& OutSize)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
		int64 ParsedSize = 0;
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const int64 Digit = Character - TEXT('0');
			if (ParsedSize > (MAX_int64 - Digit) / 10)
			{
				return false;
			}
			ParsedSize = ParsedSize * 10 + Digit;
		}
		OutSize = ParsedSize;
		return true;
	}

	ELfsPointerParseResult ParseLfsPointer(const FString& Filename, FString& OutOid, int64& OutSize)
	{
		OutOid.Reset();
		OutSize = 0;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Filename))
		{
			return ELfsPointerParseResult::InvalidPointer;
		}
		if (!Text.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")))
		{
			return ELfsPointerParseResult::NotPointer;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		bool bFoundOid = false;
		bool bFoundSize = false;
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("oid sha256:")))
			{
				if (bFoundOid)
				{
					return ELfsPointerParseResult::InvalidPointer;
				}
				OutOid = Line.Mid(11).TrimStartAndEnd();
				bFoundOid = true;
			}
			else if (Line.StartsWith(TEXT("size ")))
			{
				if (bFoundSize || !ParseLfsObjectSize(Line.Mid(5).TrimStartAndEnd(), OutSize))
				{
					return ELfsPointerParseResult::InvalidPointer;
				}
				bFoundSize = true;
			}
		}
		if (!bFoundOid || !bFoundSize || OutOid.Len() != 64)
		{
			return ELfsPointerParseResult::InvalidPointer;
		}
		for (const TCHAR Character : OutOid)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return ELfsPointerParseResult::InvalidPointer;
			}
		}
		return ELfsPointerParseResult::ValidPointer;
	}

	bool MaterializeLocalLfsObject(const FString& GitBinary, const FString& RepositoryRoot, const FString& PointerFilename,
		const FString& OutputFilename, bool& bOutNeedsFetch, FString& OutError)
	{
		bOutNeedsFetch = false;
		FString Oid;
		int64 ExpectedSize = 0;
		const ELfsPointerParseResult ParseResult = ParseLfsPointer(PointerFilename, Oid, ExpectedSize);
		if (ParseResult == ELfsPointerParseResult::NotPointer)
		{
			return true;
		}
		if (ParseResult == ELfsPointerParseResult::InvalidPointer)
		{
			OutError = TEXT("The revision contains an invalid Git LFS pointer.");
			return false;
		}

		FString StorageOutput;
		FString StorageErrors;
		FString Storage;
		if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), GitBinary, RepositoryRoot,
			{ TEXT("--path"), TEXT("--get"), TEXT("lfs.storage") }, {}, StorageOutput, StorageErrors, 0, false))
		{
			Storage = StorageOutput.TrimStartAndEnd();
		}
		if (Storage.IsEmpty())
		{
			FString CommonGitDir;
			FString CommonGitDirErrors;
			if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), GitBinary, RepositoryRoot, { TEXT("--git-common-dir") }, {}, CommonGitDir, CommonGitDirErrors))
			{
				OutError = CommonGitDirErrors.IsEmpty() ? TEXT("Could not resolve the local Git common directory for LFS.") : CommonGitDirErrors;
				return false;
			}
			Storage = FPaths::Combine(CommonGitDir.TrimStartAndEnd(), TEXT("lfs"));
		}
		if (FPaths::IsRelative(Storage))
		{
			Storage = FPaths::ConvertRelativePathToFull(RepositoryRoot, Storage);
		}
		const FString ObjectFilename = FPaths::Combine(Storage, TEXT("objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
		if (!IFileManager::Get().FileExists(*ObjectFilename))
		{
			bOutNeedsFetch = true;
			OutError = FString::Printf(TEXT("Git LFS object %s is not available locally."), *Oid);
			return false;
		}
		if (!GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, RepositoryRoot, ObjectFilename, Oid, ExpectedSize, OutError))
		{
			return false;
		}
		if (IFileManager::Get().Copy(*OutputFilename, *ObjectFilename, true, true) != COPY_OK || IFileManager::Get().FileSize(*OutputFilename) != ExpectedSize)
		{
			IFileManager::Get().Delete(*OutputFilename, false, true, true);
			OutError = FString::Printf(TEXT("Could not materialize verified Git LFS object %s."), *Oid);
			return false;
		}
		return true;
	}

	FString NormalizeRepositoryRelativePath(const FString& InPath);

	bool MakeRepositoryRelativePath(const FString& InRepositoryRoot, FString& InOutPath)
	{
		FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		FPaths::NormalizeDirectoryName(RepositoryRoot);
		if (!RepositoryRoot.EndsWith(TEXT("/")))
		{
			RepositoryRoot += TEXT("/");
		}
		FString Filename = FPaths::ConvertRelativePathToFull(InOutPath);
		FPaths::NormalizeFilename(Filename);
		if (!FPaths::IsUnderDirectory(Filename, RepositoryRoot) || !FPaths::MakePathRelativeTo(Filename, *RepositoryRoot))
		{
			return false;
		}
		InOutPath = MoveTemp(Filename);
		return true;
	}

	bool EnsureHeadBlobAvailable(const FString& GitBinary, const FString& RepositoryRoot, const FString& HeadCommitId,
		const FString& AbsoluteFilename, FString& OutError)
	{
		FString RelativePath = AbsoluteFilename;
		if (!MakeRepositoryRelativePath(RepositoryRoot, RelativePath))
		{
			OutError = FString::Printf(TEXT("Could not resolve the Git-relative path for '%s'."), *AbsoluteFilename);
			return false;
		}
		RelativePath = NormalizeRepositoryRelativePath(RelativePath);
		const FString TemporaryPointerFilename = FPaths::CreateTempFilename(*FPaths::GetPath(AbsoluteFilename), TEXT(".git-source-control-lfs-check-"), TEXT(".tmp"));
		const FString MaterializedFilename = TemporaryPointerFilename + TEXT(".materialized");
		IFileManager::Get().Delete(*TemporaryPointerFilename, false, true, true);
		bool bSuccess = false;
		if (GitSourceControlUtils::DumpRevisionBlobToFile(GitBinary, RepositoryRoot, HeadCommitId + TEXT(":") + RelativePath, TemporaryPointerFilename, OutError))
		{
			bool bNeedsFetch = false;
			bSuccess = MaterializeLocalLfsObject(GitBinary, RepositoryRoot, TemporaryPointerFilename, MaterializedFilename, bNeedsFetch, OutError);
			if (!bSuccess && bNeedsFetch && GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, RepositoryRoot, HeadCommitId, RelativePath, OutError))
			{
				bNeedsFetch = false;
				bSuccess = MaterializeLocalLfsObject(GitBinary, RepositoryRoot, TemporaryPointerFilename, MaterializedFilename, bNeedsFetch, OutError);
			}
		}
		IFileManager::Get().Delete(*TemporaryPointerFilename, false, true, true);
		IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
		return bSuccess;
	}

	bool ValidatePackageRevision(const FString& LogicalTargetFilename, const FString& MaterializedFilename, FString& OutError)
	{
		if (!LogicalTargetFilename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *MaterializedFilename) || Data.Num() < sizeof(uint32))
		{
			OutError = TEXT("The historical Unreal package is empty or unreadable.");
			return false;
		}
		const uint32 Tag = *reinterpret_cast<const uint32*>(Data.GetData());
		if (Tag != PACKAGE_FILE_TAG && Tag != PACKAGE_FILE_TAG_SWAPPED)
		{
			OutError = TEXT("The historical revision is not a valid Unreal package.");
			return false;
		}
		return true;
	}

	bool IsSupportedUAsset(const FString& Filename)
	{
		return Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase);
	}

	FString NormalizeRepositoryRelativePath(const FString& InPath)
	{
		FString Result = InPath;
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Result.StartsWith(TEXT("./")))
		{
			Result.RightChopInline(2);
		}
		return Result;
	}
}

namespace GitSourceControlAssetOperations
{
	FGitSourceControlAssetOperations::FGitSourceControlAssetOperations(FString InGitBinary, FString InRepositoryRoot)
		: GitBinary(MoveTemp(InGitBinary))
		, RepositoryRoot(FPaths::ConvertRelativePathToFull(InRepositoryRoot))
	{
		FPaths::NormalizeDirectoryName(RepositoryRoot);
	}

	FGitAssetFileFingerprint FGitSourceControlAssetOperations::CaptureFingerprint(const FString& InFilename)
	{
		FGitAssetFileFingerprint Result;
		Result.bExists = IFileManager::Get().FileExists(*InFilename);
		if (!Result.bExists)
		{
			return Result;
		}
		Result.Size = IFileManager::Get().FileSize(*InFilename);
		Result.ModifiedTime = IFileManager::Get().GetTimeStamp(*InFilename);
		TArray<uint8> Data;
		if (FFileHelper::LoadFileToArray(Data, *InFilename))
		{
			FSHA1 Sha;
			Sha.Update(Data.GetData(), Data.Num());
			Sha.Final();
			uint8 Digest[FSHA1::DigestSize];
			Sha.GetHash(Digest);
			Result.ContentHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		}
		else
		{
			Result.bHashValid = false;
		}
		return Result;
	}

	bool FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(const TArray<FString>& InFiles,
		const TArray<UPackage*>& InLoadedPackages, FString& OutError)
	{
		OutError.Reset();
		for (const FString& Filename : InFiles)
		{
			if (Filename.Contains(TEXT("__ExternalActors__"), ESearchCase::IgnoreCase) || Filename.Contains(TEXT("__ExternalObjects__"), ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("Standalone Git asset operations do not support external package paths: %s"), *Filename);
				return false;
			}
			if (!GitSourceControlAssetOperationsPrivate::IsSupportedUAsset(Filename))
			{
				OutError = FString::Printf(TEXT("Standalone Git asset operations support only tracked .uasset files; .umap support is reserved for a future release: %s"), *Filename);
				return false;
			}
		}
		for (UPackage* Package : InLoadedPackages)
		{
			if (Package == nullptr)
			{
				continue;
			}
			if (UWorld::FindWorldInPackage(Package) != nullptr)
			{
				OutError = FString::Printf(TEXT("Standalone Git asset operations do not support loaded world packages: %s"), *Package->GetName());
				return false;
			}
			if (UObject* Asset = Package->FindAssetInPackage(); Asset != nullptr && Asset->IsPackageExternal())
			{
				OutError = FString::Printf(TEXT("Standalone Git asset operations do not support loaded external packages: %s"), *Package->GetName());
				return false;
			}
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::ResolveSingleRepositoryRoot(const TArray<FString>& InFiles, const FString& InFallbackRepositoryRoot, FString& OutRepositoryRoot, FString& OutError)
	{
		OutRepositoryRoot.Reset();
		OutError.Reset();
		if (InFiles.IsEmpty())
		{
			OutError = TEXT("At least one file is required to resolve a Git repository root.");
			return false;
		}
		for (const FString& Input : InFiles)
		{
			FString Filename = FPaths::ConvertRelativePathToFull(Input);
			FPaths::NormalizeFilename(Filename);
			FString RepositoryRoot;
			if (!GitSourceControlUtils::FindRootDirectory(Filename, RepositoryRoot))
			{
				OutError = FString::Printf(TEXT("Could not resolve a Git repository for '%s'."), *Filename);
				return false;
			}
			RepositoryRoot = FPaths::ConvertRelativePathToFull(RepositoryRoot.IsEmpty() ? InFallbackRepositoryRoot : RepositoryRoot);
			FPaths::NormalizeDirectoryName(RepositoryRoot);
			if (OutRepositoryRoot.IsEmpty())
			{
				OutRepositoryRoot = MoveTemp(RepositoryRoot);
			}
			else if (!OutRepositoryRoot.Equals(RepositoryRoot, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("Selected files belong to different Git repositories: '%s' and '%s'. Run the operation separately for each repository."), *OutRepositoryRoot, *RepositoryRoot);
				return false;
			}
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::NormalizeFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles, FGitAssetOperationResult& OutResult) const
	{
		OutFiles.Reset();
		if (GitBinary.IsEmpty() || RepositoryRoot.IsEmpty() || InFiles.IsEmpty())
		{
			OutResult.AddError(TEXT("Git binary, repository root, and at least one explicit file are required."));
			return false;
		}
		TSet<FString> Seen;
		for (const FString& Input : InFiles)
		{
			FString Filename = FPaths::ConvertRelativePathToFull(Input);
			FPaths::NormalizeFilename(Filename);
			if (FPaths::DirectoryExists(Filename) || !FPaths::IsUnderDirectory(Filename, RepositoryRoot))
			{
				OutResult.FailedFiles.Add(Filename);
				OutResult.AddError(FString::Printf(TEXT("Only explicit files inside the Git repository are supported: %s"), *Filename));
				return false;
			}
			const FString Key = GitSourceControlAssetOperationsPrivate::NormalizeKey(Filename);
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				OutFiles.Add(Filename);
			}
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::QueryStates(const TArray<FString>& InFiles, TMap<FString, FGitSourceControlFileStatus>& OutStates, FGitAssetOperationResult& OutResult) const
	{
		TArray<FString> Errors;
		if (!GitSourceControlUtils::RunUpdateStatus(GitBinary, RepositoryRoot, false, InFiles, Errors, OutStates))
		{
			for (const FString& Error : Errors)
			{
				OutResult.AddError(Error);
			}
			return false;
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::Confirm(const FString& Description, const TArray<FString>& Files, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const
	{
		if (Callbacks.Confirm && !Callbacks.Confirm(Description, Files))
		{
			OutResult.bCancelled = true;
			return false;
		}
		return true;
	}

	bool PrepareForMutation(const TArray<FString>& Files, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult)
	{
		if (Callbacks.PrepareForMutation && !Callbacks.PrepareForMutation(Files))
		{
			OutResult.bCancelled = true;
			return false;
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::RecheckTargets(const TArray<FString>& Files, const TMap<FString, FGitAssetFileFingerprint>& Fingerprints,
		const FGitIndexSnapshot& IndexSnapshot, const FString& ExpectedHeadCommitId, FGitAssetOperationResult& OutResult) const
	{
		if (!ExpectedHeadCommitId.IsEmpty())
		{
			FString CurrentHeadCommitId;
			FString HeadError;
			if (!GitSourceControlAssetOperationsPrivate::ReadHeadCommitId(GitBinary, RepositoryRoot, CurrentHeadCommitId, HeadError) ||
				!CurrentHeadCommitId.Equals(ExpectedHeadCommitId, ESearchCase::CaseSensitive))
			{
				OutResult.AddError(HeadError.IsEmpty()
					? TEXT("Git HEAD changed while the confirmation dialog was open. Refresh and retry.")
					: HeadError);
				return false;
			}
		}
		for (const FString& Filename : Files)
		{
			const FGitAssetFileFingerprint* Expected = Fingerprints.Find(Filename);
			if (!Expected || !(CaptureFingerprint(Filename) == *Expected))
			{
				OutResult.AddError(FString::Printf(TEXT("The selected file changed while the operation was pending: %s"), *Filename));
				return false;
			}
		}
		FGitIndexSnapshot CurrentSnapshot;
		FString Error;
		if (!GitSourceControlUtils::CaptureIndexEntriesForPaths(GitBinary, RepositoryRoot, Files, CurrentSnapshot, Error) || !GitSourceControlAssetOperationsPrivate::SnapshotsEqual(IndexSnapshot, CurrentSnapshot))
		{
			OutResult.AddError(Error.IsEmpty() ? TEXT("The Git index changed while the operation was pending. Refresh and retry.") : Error);
			return false;
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::DiscardTrackedFiles(const TArray<FString>& InFiles, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const
	{
		OutResult = FGitAssetOperationResult();
		GitSourceControlRepositoryMutation::FGitRepositoryMutationGuard TransactionGuard(RepositoryRoot);
		if (!TransactionGuard.Acquire([&Callbacks]() { return Callbacks.IsCancellationRequested && Callbacks.IsCancellationRequested(); }))
		{
			OutResult.bCancelled = Callbacks.IsCancellationRequested && Callbacks.IsCancellationRequested();
			if (!OutResult.bCancelled)
			{
				OutResult.AddError(TEXT("Could not acquire the repository mutation transaction guard."));
			}
			return false;
		}
		TArray<FString> Files;
		if (!NormalizeFiles(InFiles, Files, OutResult)) return false;

		TMap<FString, FGitSourceControlFileStatus> States;
		if (!QueryStates(Files, States, OutResult)) return false;
		TArray<FString> RestoreFiles;
		for (const FString& Filename : Files)
		{
			if (!GitSourceControlAssetOperationsPrivate::IsSupportedUAsset(Filename))
			{
				OutResult.FailedFiles.Add(Filename);
				OutResult.AddError(FString::Printf(TEXT("Discard supports only tracked .uasset files; .umap support is reserved for a future release: %s"), *Filename));
				return false;
			}
			FGitSourceControlFileStatus* State = GitSourceControlAssetOperationsPrivate::FindState(States, Filename);
			if (!State || State->IsConflicted() || !State->IsTracked() || State->FileState == EGitFileState::Added)
			{
				OutResult.FailedFiles.Add(Filename);
				OutResult.AddError(FString::Printf(TEXT("Discard supports only non-conflicted tracked assets: %s"), *Filename));
				return false;
			}
			RestoreFiles.Add(Filename);
		}

		TMap<FString, FGitAssetFileFingerprint> Fingerprints;
		for (const FString& Filename : Files)
		{
			const FGitAssetFileFingerprint Fingerprint = CaptureFingerprint(Filename);
			if (!Fingerprint.bHashValid)
			{
				OutResult.AddError(FString::Printf(TEXT("Could not fingerprint '%s' before discard."), *Filename));
				return false;
			}
			Fingerprints.Add(Filename, Fingerprint);
		}
		FGitIndexSnapshot IndexSnapshot;
		FString Error;
		if (!GitSourceControlUtils::CaptureIndexEntriesForPaths(GitBinary, RepositoryRoot, Files, IndexSnapshot, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		FString HeadCommitId;
		if (!GitSourceControlAssetOperationsPrivate::ReadHeadCommitId(GitBinary, RepositoryRoot, HeadCommitId, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		if (!Confirm(TEXT("Discard the selected tracked Git changes."), Files, Callbacks, OutResult))
		{
			return false;
		}
		if (!PrepareForMutation(Files, Callbacks, OutResult))
		{
			return false;
		}
		auto ReloadAfterPreparedFailure = [&Callbacks, &Files]()
		{
			if (Callbacks.ReloadPackages)
			{
				Callbacks.ReloadPackages(Files);
			}
		};
#if WITH_DEV_AUTOMATION_TESTS
		if (Callbacks.BeforeCommitPointForTesting)
		{
			Callbacks.BeforeCommitPointForTesting();
		}
#endif
		if (!RecheckTargets(Files, Fingerprints, IndexSnapshot, HeadCommitId, OutResult))
		{
			ReloadAfterPreparedFailure();
			return false;
		}
		FString CommitPointError;
		if (!FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(Files, {}, CommitPointError))
		{
			OutResult.AddError(CommitPointError);
			ReloadAfterPreparedFailure();
			return false;
		}
		for (const FString& Filename : RestoreFiles)
		{
			if (!GitSourceControlAssetOperationsPrivate::EnsureHeadBlobAvailable(GitBinary, RepositoryRoot, HeadCommitId, Filename, Error))
			{
				OutResult.AddError(Error.IsEmpty() ? TEXT("Could not validate the HEAD package topology before discard.") : Error);
				ReloadAfterPreparedFailure();
				return false;
			}
		}
		TArray<GitSourceControlAssetOperationsPrivate::FFileBackup> Backups;
		if (!GitSourceControlAssetOperationsPrivate::CreateBackups(Files, Backups, Error))
		{
			OutResult.AddError(Error);
			ReloadAfterPreparedFailure();
			return false;
		}
		const bool bMutated = GitSourceControlUtils::RunExactPathspecMutation(GitBinary, RepositoryRoot, TEXT("restore"), { TEXT("--source=HEAD"), TEXT("--staged"), TEXT("--worktree") }, RestoreFiles, Error);
		if (!bMutated)
		{
			TArray<FString> FailedBackups;
			const bool bWorktreeRestored = GitSourceControlAssetOperationsPrivate::RestoreBackups(Backups, FailedBackups);
			FString RestoreError;
			GitSourceControlUtils::RestoreIndexEntries(GitBinary, RepositoryRoot, IndexSnapshot, RestoreError);
			OutResult.AddError(Error);
			if (!RestoreError.IsEmpty()) OutResult.AddError(RestoreError);
			if (bWorktreeRestored)
			{
				GitSourceControlAssetOperationsPrivate::DeleteBackups(Backups);
			}
			else
			{
				OutResult.AddError(FString::Printf(TEXT("Worktree rollback was incomplete. Safety backups were preserved at:\n%s"), *FString::Join(FailedBackups, TEXT("\n"))));
			}
			return false;
		}
		GitSourceControlAssetOperationsPrivate::DeleteBackups(Backups);
		OutResult.bSucceeded = true;
		OutResult.AffectedFiles = Files;
		if (Callbacks.ReloadPackages)
		{
			OutResult.bReloadSucceeded = Callbacks.ReloadPackages(Files);
		}
		return true;
	}

	bool FGitSourceControlAssetOperations::RestoreRevisionToWorkspace(const FString& InCurrentFilename, const FString& InCommitId, const FString& InHistoricalPath,
		const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const
	{
		OutResult = FGitAssetOperationResult();
		GitSourceControlRepositoryMutation::FGitRepositoryMutationGuard TransactionGuard(RepositoryRoot);
		if (!TransactionGuard.Acquire([&Callbacks]() { return Callbacks.IsCancellationRequested && Callbacks.IsCancellationRequested(); }))
		{
			OutResult.bCancelled = Callbacks.IsCancellationRequested && Callbacks.IsCancellationRequested();
			if (!OutResult.bCancelled)
			{
				OutResult.AddError(TEXT("Could not acquire the repository mutation transaction guard."));
			}
			return false;
		}
		TArray<FString> Files;
		if (!NormalizeFiles({ InCurrentFilename }, Files, OutResult)) return false;
		const FString Target = Files[0];
		if (!GitSourceControlAssetOperationsPrivate::IsSupportedUAsset(Target))
		{
			OutResult.AddError(TEXT("Historical restore supports only tracked .uasset files; .umap support is reserved for a future release."));
			return false;
		}
		bool bValidCommitId = InCommitId.Len() == 40 || InCommitId.Len() == 64;
		for (const TCHAR Character : InCommitId)
		{
			bValidCommitId &= FChar::IsHexDigit(Character);
		}
		FString HistoricalPath = InHistoricalPath;
		HistoricalPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!bValidCommitId || HistoricalPath.IsEmpty() || HistoricalPath.StartsWith(TEXT("/")) || HistoricalPath.Contains(TEXT("../")) || HistoricalPath.Contains(TEXT("..\\")))
		{
			OutResult.AddError(TEXT("A complete commit id and historical path are required."));
			return false;
		}
		FString CurrentRelativePath = Target;
		if (!GitSourceControlAssetOperationsPrivate::MakeRepositoryRelativePath(RepositoryRoot, CurrentRelativePath))
		{
			OutResult.AddError(TEXT("Could not resolve the current Git-relative asset path."));
			return false;
		}
		CurrentRelativePath = GitSourceControlAssetOperationsPrivate::NormalizeRepositoryRelativePath(CurrentRelativePath);
		HistoricalPath = GitSourceControlAssetOperationsPrivate::NormalizeRepositoryRelativePath(HistoricalPath);
		// Git path identity is case-sensitive even when the workspace filesystem is not.
		if (!HistoricalPath.Equals(CurrentRelativePath, ESearchCase::CaseSensitive))
		{
			OutResult.AddError(TEXT("Historical restore is supported only when the revision path matches the current Git path. Use Diff or Fetch for a renamed revision."));
			return false;
		}
		FString ResolvedCommitId;
		FString Error;
		if (!GitSourceControlAssetOperationsPrivate::ResolveFullCommitId(GitBinary, RepositoryRoot, InCommitId, ResolvedCommitId, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		const FGitAssetFileFingerprint Fingerprint = CaptureFingerprint(Target);
		if (!Fingerprint.bHashValid)
		{
			OutResult.AddError(FString::Printf(TEXT("Could not fingerprint '%s' before historical restore."), *Target));
			return false;
		}
		FGitIndexSnapshot IndexSnapshot;
		if (!GitSourceControlUtils::CaptureIndexEntriesForPaths(GitBinary, RepositoryRoot, Files, IndexSnapshot, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		FString HeadCommitId;
		if (!GitSourceControlAssetOperationsPrivate::ReadHeadCommitId(GitBinary, RepositoryRoot, HeadCommitId, Error))
		{
			OutResult.AddError(Error);
			return false;
		}
		const FString TemporaryFilename = FPaths::CreateTempFilename(*FPaths::GetPath(Target), TEXT(".git-source-control-txn-revision-"), TEXT(".tmp"));
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		const FString RevisionSpec = ResolvedCommitId + TEXT(":") + HistoricalPath;
		if (!GitSourceControlUtils::DumpRevisionBlobToFile(GitBinary, RepositoryRoot, RevisionSpec, TemporaryFilename, Error))
		{
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			OutResult.AddError(Error);
			return false;
		}
		const FString MaterializedFilename = TemporaryFilename + TEXT(".lfs");
		bool bNeedsLfsFetch = false;
		bool bMaterialized = GitSourceControlAssetOperationsPrivate::MaterializeLocalLfsObject(
			GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsLfsFetch, Error);
		if (!bMaterialized && bNeedsLfsFetch &&
			GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, RepositoryRoot, ResolvedCommitId, HistoricalPath, Error))
		{
			bMaterialized = GitSourceControlAssetOperationsPrivate::MaterializeLocalLfsObject(
				GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsLfsFetch, Error);
		}
		if (!bMaterialized)
		{
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
			OutResult.AddError(Error);
			return false;
		}
		if (IFileManager::Get().FileExists(*MaterializedFilename))
		{
			if (!IFileManager::Get().Move(*TemporaryFilename, *MaterializedFilename, true, true, false, true))
			{
				IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
				IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
				OutResult.AddError(TEXT("Failed to materialize the local Git LFS object."));
				return false;
			}
		}
		if (!GitSourceControlAssetOperationsPrivate::ValidatePackageRevision(Target, TemporaryFilename, Error))
		{
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			OutResult.AddError(Error);
			return false;
		}
		TMap<FString, FGitAssetFileFingerprint> ExpectedFingerprints;
		ExpectedFingerprints.Add(Target, Fingerprint);
		if (!Confirm(FString::Printf(TEXT("Restore revision %s to the workspace. The Git index will remain unchanged."), *ResolvedCommitId.Left(12)), Files, Callbacks, OutResult))
		{
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		if (!PrepareForMutation(Files, Callbacks, OutResult))
		{
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		auto ReloadAfterPreparedFailure = [&Callbacks, &Files]()
		{
			if (Callbacks.ReloadPackages)
			{
				Callbacks.ReloadPackages(Files);
			}
		};
#if WITH_DEV_AUTOMATION_TESTS
		if (Callbacks.BeforeCommitPointForTesting)
		{
			Callbacks.BeforeCommitPointForTesting();
		}
#endif
		if (!RecheckTargets(Files, ExpectedFingerprints, IndexSnapshot, HeadCommitId, OutResult))
		{
			ReloadAfterPreparedFailure();
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		FString CommitPointError;
		if (!FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(Files, {}, CommitPointError))
		{
			OutResult.AddError(CommitPointError);
			ReloadAfterPreparedFailure();
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		TArray<GitSourceControlAssetOperationsPrivate::FFileBackup> Backups;
		if (!GitSourceControlAssetOperationsPrivate::CreateBackups(Files, Backups, Error))
		{
			OutResult.AddError(Error);
			ReloadAfterPreparedFailure();
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		if (!GitSourceControlUtils::RunExactPathspecMutation(GitBinary, RepositoryRoot, TEXT("reset"), { TEXT("-q"), TEXT("HEAD") }, Files, Error))
		{
			FString RestoreError;
			GitSourceControlUtils::RestoreIndexEntries(GitBinary, RepositoryRoot, IndexSnapshot, RestoreError);
			GitSourceControlAssetOperationsPrivate::DeleteBackups(Backups);
			OutResult.AddError(Error);
			if (!RestoreError.IsEmpty())
			{
				OutResult.AddError(RestoreError);
			}
			ReloadAfterPreparedFailure();
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			return false;
		}
		bool bCanReplaceWorktree = true;
#if WITH_DEV_AUTOMATION_TESTS
		if (Callbacks.AllowWorktreeReplaceForTesting)
		{
			bCanReplaceWorktree = Callbacks.AllowWorktreeReplaceForTesting();
		}
#endif
		if (!bCanReplaceWorktree || !IFileManager::Get().Move(*Target, *TemporaryFilename, true, true, false, true))
		{
			TArray<FString> FailedBackups;
			const bool bWorktreeRestored = GitSourceControlAssetOperationsPrivate::RestoreBackups(Backups, FailedBackups);
			FString RestoreError;
			GitSourceControlUtils::RestoreIndexEntries(GitBinary, RepositoryRoot, IndexSnapshot, RestoreError);
			if (bWorktreeRestored)
			{
				GitSourceControlAssetOperationsPrivate::DeleteBackups(Backups);
			}
			else
			{
				OutResult.AddError(FString::Printf(TEXT("Workspace rollback was incomplete. Safety backups were preserved at:\n%s"), *FString::Join(FailedBackups, TEXT("\n"))));
			}
			if (!RestoreError.IsEmpty())
			{
				OutResult.AddError(RestoreError);
			}
			ReloadAfterPreparedFailure();
			IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
			OutResult.AddError(bCanReplaceWorktree
				? FString::Printf(TEXT("Could not replace workspace file '%s'."), *Target)
				: TEXT("Worktree replacement was rejected by the force-restore rollback test seam."));
			return false;
		}
		GitSourceControlAssetOperationsPrivate::DeleteBackups(Backups);
		OutResult.bSucceeded = true;
		OutResult.AffectedFiles = Files;
		if (Callbacks.ReloadPackages) OutResult.bReloadSucceeded = Callbacks.ReloadPackages(Files);
		return true;
	}
}
