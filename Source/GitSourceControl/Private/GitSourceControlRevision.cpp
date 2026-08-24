// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlRevision.h"

#include "Algo/AllOf.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlUtils.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlRevisionPrivate
{
	constexpr double LfsBridgeWaitTimeoutSeconds = 100.0;
	bool IsLfsPointerFile(const FString& Filename)
	{
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *Filename))
		{
			return false;
		}

		static constexpr ANSICHAR Signature[] = "version https://git-lfs.github.com/spec/v1";
		return Data.Num() >= UE_ARRAY_COUNT(Signature) - 1
			&& FMemory::Memcmp(Data.GetData(), Signature, UE_ARRAY_COUNT(Signature) - 1) == 0;
	}

	bool IsPackageFile(const FString& Filename)
	{
		return Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Filename.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase);
	}

	bool HasValidPackageHeader(const FString& Filename)
	{
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *Filename) || Data.Num() < sizeof(uint32))
		{
			return false;
		}

		const uint32 Tag = *reinterpret_cast<const uint32*>(Data.GetData());
		return Tag == PACKAGE_FILE_TAG || Tag == PACKAGE_FILE_TAG_SWAPPED;
	}

	bool ParseLfsPointer(const FString& Filename, FString& OutOid, int64& OutSize)
	{
		OutOid.Reset();
		OutSize = 0;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Filename))
		{
			return false;
		}

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		bool bFoundOid = false;
		bool bFoundSize = false;
		for (const FString& Line : Lines)
		{
			static const FString Prefix(TEXT("oid sha256:"));
			if (Line.StartsWith(TEXT("oid sha256:"), ESearchCase::CaseSensitive))
			{
				if (bFoundOid)
				{
					return false;
				}
				OutOid = Line.Mid(Prefix.Len()).TrimStartAndEnd();
				bFoundOid = true;
			}
			else if (Line.StartsWith(TEXT("size "), ESearchCase::CaseSensitive))
			{
				if (bFoundSize)
				{
					return false;
				}
				const FString SizeValue = Line.Mid(5).TrimStartAndEnd();
				if (SizeValue.IsEmpty())
				{
					return false;
				}
				for (const TCHAR Character : SizeValue)
				{
					if (Character < TEXT('0') || Character > TEXT('9'))
					{
						return false;
					}
				}
				OutSize = FCString::Atoi64(*SizeValue);
				bFoundSize = true;
			}
		}
		return bFoundOid && bFoundSize && OutOid.Len() == 64 && Algo::AllOf(OutOid, [](TCHAR Character)
		{
			return FChar::IsHexDigit(Character);
		});
	}

	bool MaterializeLocalLfsObject(const FString& GitBinary, const FString& RepositoryRoot, const FString& PointerFilename,
		const FString& Destination, bool& bOutNeedsFetch, FString& OutError)
	{
		bOutNeedsFetch = false;
		OutError.Reset();
		FString Oid;
		int64 ExpectedSize = 0;
		if (!ParseLfsPointer(PointerFilename, Oid, ExpectedSize))
		{
			OutError = TEXT("The revision contains an invalid Git LFS pointer.");
			return false;
		}

		FString StorageOutput;
		FString StorageErrors;
		FString LfsStorage;
		if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), GitBinary, RepositoryRoot,
			{ TEXT("--path"), TEXT("--get"), TEXT("lfs.storage") }, FGitSourceControlModule::GetEmptyStringArray(),
			StorageOutput, StorageErrors, 0, false))
		{
			LfsStorage = MoveTemp(StorageOutput);
			LfsStorage.TrimStartAndEndInline();
		}

		if (LfsStorage.IsEmpty())
		{
			FString CommonGitDir;
			FString Errors;
			if (!GitSourceControlUtils::RunCommandInternalRaw(
				TEXT("rev-parse"), GitBinary, RepositoryRoot, { TEXT("--git-common-dir") },
				FGitSourceControlModule::GetEmptyStringArray(), CommonGitDir, Errors))
			{
				OutError = Errors.IsEmpty() ? TEXT("Could not resolve local Git LFS storage.") : Errors;
				return false;
			}
			CommonGitDir.TrimStartAndEndInline();
			LfsStorage = FPaths::Combine(CommonGitDir, TEXT("lfs"));
		}
		if (FPaths::IsRelative(LfsStorage))
		{
			LfsStorage = FPaths::ConvertRelativePathToFull(RepositoryRoot, LfsStorage);
		}

		const FString ObjectPath = FPaths::Combine(LfsStorage, TEXT("objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
		if (!IFileManager::Get().FileExists(*ObjectPath))
		{
			bOutNeedsFetch = true;
			OutError = FString::Printf(TEXT("Git LFS object %s is not available locally."), *Oid);
			return false;
		}
		FString VerificationError;
		if (!GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, RepositoryRoot, ObjectPath, Oid, ExpectedSize, VerificationError))
		{
			OutError = VerificationError;
			return false;
		}

		if (IFileManager::Get().Copy(*Destination, *ObjectPath, true, true) != COPY_OK)
		{
			OutError = TEXT("Could not materialize the verified Git LFS object.");
			return false;
		}
		return true;
	}

	FString MakeRevisionTempFilename(const FString& CommitId, const FString& Filename)
	{
		FSHA1 Sha;
		Sha.Update(reinterpret_cast<const uint8*>(*Filename), Filename.Len() * sizeof(TCHAR));
		Sha.Final();
		uint8 Hash[FSHA1::DigestSize];
		Sha.GetHash(Hash);
		return FPaths::Combine(FPaths::DiffDir(), FString::Printf(TEXT("git-%s-%s-%s"), *CommitId, *BytesToHex(Hash, UE_ARRAY_COUNT(Hash)), *FPaths::GetCleanFilename(Filename)));
	}

	struct FLfsFetchBridgeState final
	{
		FLfsFetchBridgeState()
			: CancellationContext(MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>())
			, CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
		{
		}

		~FLfsFetchBridgeState()
		{
			if (CompletionEvent != nullptr)
			{
				FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
				CompletionEvent = nullptr;
			}
		}

		void Complete(const bool bInSuccess, const FString& InError)
		{
			{
				FScopeLock Lock(&ResultLock);
				bSuccess = bInSuccess;
				Error = InError;
			}
			if (CompletionEvent != nullptr)
			{
				CompletionEvent->Trigger();
			}
		}

		void ReadResult(bool& bOutSuccess, FString& OutError)
		{
			FScopeLock Lock(&ResultLock);
			bOutSuccess = bSuccess;
			OutError = Error;
		}

		TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
		FEvent* CompletionEvent = nullptr;
		FCriticalSection ResultLock;
		bool bSuccess = false;
		FString Error;
		TAtomic<bool> bAbandoned = false;
	};

	bool FetchMissingLfsContent(const FString& GitBinary, const FString& RepositoryRoot, const FString& CommitId, const FString& HistoricalPath, FString& OutError)
	{
		const bool bCanShowProgress = !FApp::IsUnattended() && !IsRunningCommandlet() && FSlateApplication::IsInitialized();
		if (!bCanShowProgress)
		{
			const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
				MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
			GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
			return GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, RepositoryRoot, CommitId, HistoricalPath, OutError);
		}

		const TSharedRef<FLfsFetchBridgeState, ESPMode::ThreadSafe> Bridge = MakeShared<FLfsFetchBridgeState, ESPMode::ThreadSafe>();
		auto RunFetchOnGameThread = [GitBinary, RepositoryRoot, CommitId, HistoricalPath, Bridge]()
		{
			if (Bridge->bAbandoned.Load())
			{
				Bridge->Complete(false, TEXT("Git LFS download was cancelled before the progress dialog could start."));
				return;
			}

			FScopedSlowTask SlowTask(1.0f, LOCTEXT("FetchingLfsRevision", "Downloading required Git LFS revision content..."), true);
			SlowTask.MakeDialog(true);
			const TSharedRef<FString, ESPMode::ThreadSafe> FetchError = MakeShared<FString, ESPMode::ThreadSafe>();
			TFuture<bool> FetchFuture = Async(EAsyncExecution::ThreadPool, [GitBinary, RepositoryRoot, CommitId, HistoricalPath, Bridge, FetchError]()
			{
				GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(Bridge->CancellationContext);
				const bool bSuccess = GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, RepositoryRoot, CommitId, HistoricalPath, *FetchError);
				return bSuccess;
			});
			while (!FetchFuture.IsReady())
			{
				if (SlowTask.ShouldCancel())
				{
					Bridge->CancellationContext->Cancel();
				}
				FSlateApplication::Get().PumpMessages();
				FPlatformProcess::Sleep(0.01f);
			}
			const bool bSucceeded = FetchFuture.Get();
			Bridge->Complete(bSucceeded, *FetchError);
		};

		if (IsInGameThread())
		{
			RunFetchOnGameThread();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [RunFetchOnGameThread]() mutable
			{
				RunFetchOnGameThread();
			});
			if (Bridge->CompletionEvent == nullptr || !Bridge->CompletionEvent->Wait(static_cast<uint32>(LfsBridgeWaitTimeoutSeconds * 1000.0)))
			{
				Bridge->bAbandoned.Store(true);
				Bridge->CancellationContext->Cancel();
				OutError = TEXT("Git LFS download did not start or finish before the 100 second safety timeout.");
				return false;
			}
		}

		bool bSucceeded = false;
		Bridge->ReadResult(bSucceeded, OutError);
		if (!bSucceeded && !Bridge->CancellationContext->IsCancellationRequested())
		{
			const FString ErrorForDialog = OutError;
			auto ShowFailureDialog = [ErrorForDialog]()
			{
				FMessageDialog::Open(EAppMsgType::Ok,
					FText::Format(LOCTEXT("FetchLfsRevisionFailed", "Could not download the required Git LFS revision content.\n\n{0}"), FText::FromString(ErrorForDialog)),
					LOCTEXT("FetchLfsRevisionFailedTitle", "Git LFS Download Failed"));
			};
			if (IsInGameThread())
			{
				ShowFailureDialog();
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [ShowFailureDialog]() mutable
				{
					ShowFailureDialog();
				});
			}
		}
		return bSucceeded;
	}
}

#if ENGINE_MAJOR_VERSION >= 5
bool FGitSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (InConcurrency != EConcurrency::Synchronous)
	{
		UE_LOG(LogSourceControl, Verbose, TEXT("Revision export is synchronous because Unreal requires the completed file immediately."));
	}
#else
bool FGitSourceControlRevision::Get(FString& InOutFilename) const
{
#endif
	if (InOutFilename.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*FPaths::DiffDir(), true);
		InOutFilename = FPaths::ConvertRelativePathToFull(GitSourceControlRevisionPrivate::MakeRevisionTempFilename(CommitId, Filename));
	}

	return ExportToFile(InOutFilename);
}

bool FGitSourceControlRevision::ExportToFile(const FString& InFilename) const
{
	const FGitSourceControlModule* Module = FGitSourceControlModule::GetThreadSafe();
	if (!Module || CommitId.IsEmpty() || Filename.IsEmpty())
	{
		return false;
	}

	const FGitSourceControlProvider& Provider = Module->GetProvider();
	const FString RepositoryRoot = PathToRepoRoot.IsEmpty() ? Provider.GetPathToRepositoryRoot() : PathToRepoRoot;
	const FString& GitBinary = Provider.GetGitBinaryPath();
	const FString TemporaryFilename = InFilename + TEXT(".git-export-tmp");
	IFileManager::Get().Delete(*TemporaryFilename, false, true, true);

	const FString RevisionSpec = FString::Printf(TEXT("%s:%s"), *CommitId, *Filename);
	FString ExportError;
	bool bSuccess = GitSourceControlUtils::DumpRevisionBlobToFile(GitBinary, RepositoryRoot, RevisionSpec, TemporaryFilename, ExportError);
	if (bSuccess && GitSourceControlRevisionPrivate::IsLfsPointerFile(TemporaryFilename))
	{
		const FString MaterializedFilename = TemporaryFilename + TEXT(".lfs");
		bool bNeedsFetch = false;
		bSuccess = GitSourceControlRevisionPrivate::MaterializeLocalLfsObject(GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsFetch, ExportError);
		if (!bSuccess && bNeedsFetch && GitSourceControlRevisionPrivate::FetchMissingLfsContent(GitBinary, RepositoryRoot, CommitId, Filename, ExportError))
		{
			bSuccess = GitSourceControlRevisionPrivate::MaterializeLocalLfsObject(GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsFetch, ExportError);
		}
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		if (bSuccess)
		{
			bSuccess = IFileManager::Get().Move(*TemporaryFilename, *MaterializedFilename, true, true, false, true);
		}
		else
		{
			IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
			UE_LOG(LogSourceControl, Warning, TEXT("Git LFS revision export failed for '%s': %s"), *Filename, *ExportError);
		}
	}

	if (bSuccess && GitSourceControlRevisionPrivate::IsPackageFile(Filename) && !GitSourceControlRevisionPrivate::HasValidPackageHeader(TemporaryFilename))
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Git revision export for '%s' is not a valid Unreal package."), *Filename);
		bSuccess = false;
	}

	if (!bSuccess)
	{
		if (!ExportError.IsEmpty())
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Git revision export failed for '%s': %s"), *Filename, *ExportError);
		}
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		return false;
	}

	return IFileManager::Get().Move(*InFilename, *TemporaryFilename, true, true, false, true);
}

bool FGitSourceControlRevision::GetAnnotated(TArray<FAnnotationLine>& OutLines) const
{
	return false;
}

bool FGitSourceControlRevision::GetAnnotated(FString& InOutFilename) const
{
	return false;
}

const FString& FGitSourceControlRevision::GetFilename() const
{
	return LocalFilename.IsEmpty() ? Filename : LocalFilename;
}

int32 FGitSourceControlRevision::GetRevisionNumber() const
{
	return RevisionNumber;
}

const FString& FGitSourceControlRevision::GetRevision() const
{
	return ShortCommitId;
}

const FString& FGitSourceControlRevision::GetDescription() const
{
	return Description;
}

const FString& FGitSourceControlRevision::GetUserName() const
{
	return UserName;
}

const FString& FGitSourceControlRevision::GetClientSpec() const
{
	static const FString EmptyString;
	return EmptyString;
}

const FString& FGitSourceControlRevision::GetAction() const
{
	return Action;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlRevision::GetBranchSource() const
{
	return nullptr;
}

const FDateTime& FGitSourceControlRevision::GetDate() const
{
	return Date;
}

int32 FGitSourceControlRevision::GetCheckInIdentifier() const
{
	return CommitIdNumber;
}

int32 FGitSourceControlRevision::GetFileSize() const
{
	return FileSize;
}

#undef LOCTEXT_NAMESPACE
