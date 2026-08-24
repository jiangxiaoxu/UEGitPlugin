// Copyright (c) 2026

#include "GitLocalSourceControl.h"

#include "Async/Async.h"
#include "GitSourceControlAssetOperations.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlRevision.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Async/TaskGraphInterfaces.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#endif

struct FGitLocalSourceControlOperationState final
{
	bool RequestShutdownCancellationIfAllowed()
	{
		FScopeLock Lock(&Mutex);
		if (bWorkerFinished || !bCancellationAllowed)
		{
			return false;
		}
		CancellationContext->Cancel();
		return true;
	}

	FCriticalSection Mutex;
	TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	EGitLocalSourceControlOperationPhase Phase = EGitLocalSourceControlOperationPhase::Queued;
	FGitLocalSourceControlOperationResult Result;
	TArray<FString> GuardErrors;
	bool bCancellationAllowed = true;
	bool bWorkerFinished = false;
};

namespace GitLocalSourceControlPrivate
{
	class FOperationRegistry final
	{
	public:
		FOperationRegistry()
			: AllWorkersFinished(FPlatformProcess::GetSynchEventFromPool(true))
		{
			AllWorkersFinished->Trigger();
		}

		~FOperationRegistry()
		{
			FPlatformProcess::ReturnSynchEventToPool(AllWorkersFinished);
		}

		bool TryBegin(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingNewWorkers)
			{
				InState->CancellationContext->Cancel();
				return false;
			}
			ActiveStates.Add(InState);
			AllWorkersFinished->Reset();
			return true;
		}

		void End(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
		{
			FScopeLock Lock(&Mutex);
			ActiveStates.RemoveSingleSwap(InState, EAllowShrinking::No);
			if (ActiveStates.IsEmpty())
			{
				AllWorkersFinished->Trigger();
			}
		}

		void Shutdown()
		{
			TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> StatesToCancel;
			{
				FScopeLock Lock(&Mutex);
				bAcceptingNewWorkers = false;
				StatesToCancel = ActiveStates;
			}
			for (const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& State : StatesToCancel)
			{
				State->RequestShutdownCancellationIfAllowed();
			}

			while (!AllWorkersFinished->Wait(10))
			{
				if (IsInGameThread())
				{
					FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				}
			}
		}

	private:
		FCriticalSection Mutex;
		FEvent* AllWorkersFinished = nullptr;
		bool bAcceptingNewWorkers = true;
		TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> ActiveStates;
	};

	class FRepositoryMutationGuard final
	{
	public:
		FRepositoryMutationGuard(const FString& InRepositoryRoot, const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
			: Mutex(GetMutex(InRepositoryRoot))
			, State(InState)
		{
		}

		~FRepositoryMutationGuard()
		{
			if (bLocked)
			{
				Mutex->Unlock();
			}
		}

		bool Acquire()
		{
			while (!Mutex->TryLock())
			{
				if (State->CancellationContext->IsCancellationRequested())
				{
					return false;
				}
				FPlatformProcess::SleepNoStats(0.005f);
			}
			bLocked = true;
			return !State->CancellationContext->IsCancellationRequested();
		}

	private:
		static TSharedRef<FCriticalSection, ESPMode::ThreadSafe> GetMutex(const FString& InRepositoryRoot)
		{
			const FString Key = InRepositoryRoot.ToLower();
			FScopeLock Lock(&RepositoryLocksMutex);
			if (const TSharedRef<FCriticalSection, ESPMode::ThreadSafe>* Existing = RepositoryLocks.Find(Key))
			{
				return *Existing;
			}
			const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> NewMutex = MakeShared<FCriticalSection, ESPMode::ThreadSafe>();
			RepositoryLocks.Add(Key, NewMutex);
			return NewMutex;
		}

		static FCriticalSection RepositoryLocksMutex;
		static TMap<FString, TSharedRef<FCriticalSection, ESPMode::ThreadSafe>> RepositoryLocks;

		TSharedRef<FCriticalSection, ESPMode::ThreadSafe> Mutex;
		TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State;
		bool bLocked = false;
	};

	FCriticalSection FRepositoryMutationGuard::RepositoryLocksMutex;
	TMap<FString, TSharedRef<FCriticalSection, ESPMode::ThreadSafe>> FRepositoryMutationGuard::RepositoryLocks;

	FOperationRegistry& GetOperationRegistry()
	{
		static FOperationRegistry Registry;
		return Registry;
	}

	class FGameThreadBridge final
	{
	public:
		FGameThreadBridge()
			: CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
		{
		}

		~FGameThreadBridge()
		{
			FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		}

		void Complete(const bool bInResult)
		{
			{
				FScopeLock Lock(&Mutex);
				bResult = bInResult;
			}
			CompletionEvent->Trigger();
		}

		bool Wait() const
		{
			CompletionEvent->Wait();
			FScopeLock Lock(&Mutex);
			return bResult;
		}

	private:
		mutable FCriticalSection Mutex;
		FEvent* CompletionEvent = nullptr;
		bool bResult = false;
	};

	bool InvokeOnGameThreadAndWait(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, TFunction<bool()>&& InWork)
	{
		if (IsInGameThread())
		{
			return !InState->CancellationContext->IsCancellationRequested() && InWork();
		}

		const TSharedRef<FGameThreadBridge, ESPMode::ThreadSafe> Bridge = MakeShared<FGameThreadBridge, ESPMode::ThreadSafe>();
		AsyncTask(ENamedThreads::GameThread, [InState, Bridge, Work = MoveTemp(InWork)]() mutable
		{
			Bridge->Complete(!InState->CancellationContext->IsCancellationRequested() && Work());
		});
		return Bridge->Wait();
	}

	enum class ETargetAccess : uint8
	{
		ReadOnly,
		Mutation,
	};
	struct FResolvedTarget final
	{
		FString ObjectPath;
		FString PackageName;
		FString Filename;
		FString GitBinary;
		FString RepositoryRoot;
	};

	FString NormalizeObjectPath(FString InObjectPath)
	{
		InObjectPath.TrimStartAndEndInline();
		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (InObjectPath.FindChar(TEXT('\''), FirstQuote) && InObjectPath.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			InObjectPath = InObjectPath.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
		}
		return InObjectPath;
	}

	void AppendProviderErrors(FGitLocalSourceControlProviderInfo& InOutInfo, const FGitSourceControlProvider& InProvider)
	{
		TArray<FString> Errors;
		for (const FText& Error : InProvider.GetLastErrors())
		{
			if (!Error.IsEmpty())
			{
				Errors.Add(Error.ToString());
			}
		}
		InOutInfo.Error = FString::Join(Errors, TEXT("\n"));
	}

	FGitLocalSourceControlProviderInfo GetProviderInfoOnGameThread()
	{
		check(IsInGameThread());
		FGitLocalSourceControlProviderInfo Info;
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
		{
			Info.Error = TEXT("GitSourceControl is not loaded.");
			return Info;
		}

		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		if (!Provider.IsGitAvailable())
		{
			Provider.CheckGitAvailability();
		}
		if (Provider.GetPathToGitRoot().IsEmpty())
		{
			Provider.CheckRepositoryStatus();
		}

		Info.bAvailable = Provider.IsGitAvailable() && !Provider.GetPathToGitRoot().IsEmpty();
		Info.GitBinary = Provider.GetGitBinaryPath();
		Info.RepositoryRoot = Provider.GetPathToGitRoot();
		AppendProviderErrors(Info, Provider);
		if (!Info.bAvailable && Info.Error.IsEmpty())
		{
			Info.Error = TEXT("The local Git provider could not resolve a Git binary and repository root.");
		}
		return Info;
	}

	bool ResolveTarget(const FString& InAssetObjectPath, const ETargetAccess InAccess, FResolvedTarget& OutTarget, FString& OutError)
	{
		check(IsInGameThread());
		OutTarget = FResolvedTarget();
		OutError.Reset();

		const FString ObjectPath = NormalizeObjectPath(InAssetObjectPath);
		if (ObjectPath.IsEmpty())
		{
			OutError = TEXT("An asset object path is required.");
			return false;
		}

		const FSoftObjectPath SoftObjectPath(ObjectPath);
		FString PackageName = SoftObjectPath.GetLongPackageName();
		if (PackageName.IsEmpty())
		{
			PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName, false))
		{
			OutError = FString::Printf(TEXT("The input is not an asset object path: %s"), *InAssetObjectPath);
			return false;
		}

		FString Filename;
		const bool bPackageExists = FPackageName::DoesPackageExist(PackageName, &Filename);
		if (!bPackageExists)
		{
			// `git log --follow` and an atomic rename discard must be able to address a
			// tracked source path which was deleted or moved out of the worktree.
			Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		}
		Filename = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Filename);
		if (Filename.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("Map and World Partition assets are not available through the scripted restore API.");
			return false;
		}
		if (!Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Only .uasset files are accepted by the scripted asset API: %s"), *Filename);
			return false;
		}
		if (InAccess == ETargetAccess::Mutation)
		{
			if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
			{
				OutError = ExistingPackage->IsDirty()
					? FString::Printf(TEXT("The asset package has unsaved Editor changes and cannot be mutated by automation: %s"), *PackageName)
					: FString::Printf(TEXT("The asset package is loaded in the Editor and cannot be mutated by automation: %s"), *PackageName);
				return false;
			}
		}

		const FGitLocalSourceControlProviderInfo ProviderInfo = GetProviderInfoOnGameThread();
		if (!ProviderInfo.bAvailable)
		{
			OutError = ProviderInfo.Error;
			return false;
		}
		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		const FString RepositoryRoot = Provider.ResolveRepositoryRootForFile(Filename);
		if (RepositoryRoot.IsEmpty())
		{
			OutError = FString::Printf(TEXT("The asset file is not in a connected local Git repository: %s"), *Filename);
			return false;
		}

		OutTarget.ObjectPath = ObjectPath;
		OutTarget.PackageName = PackageName;
		OutTarget.Filename = MoveTemp(Filename);
		OutTarget.GitBinary = ProviderInfo.GitBinary;
		OutTarget.RepositoryRoot = RepositoryRoot;
		return true;
	}

	bool ResolveTargets(const TArray<FString>& InAssetObjectPaths, const ETargetAccess InAccess, TArray<FResolvedTarget>& OutTargets, FString& OutError)
	{
		check(IsInGameThread());
		OutTargets.Reset();
		OutError.Reset();
		if (InAssetObjectPaths.IsEmpty())
		{
			OutError = TEXT("At least one asset object path is required.");
			return false;
		}

		TSet<FString> SeenFilenames;
		for (const FString& ObjectPath : InAssetObjectPaths)
		{
			FResolvedTarget Target;
			if (!ResolveTarget(ObjectPath, InAccess, Target, OutError))
			{
				return false;
			}
			const FString Key = Target.Filename.ToLower();
			if (!SeenFilenames.Contains(Key))
			{
				SeenFilenames.Add(Key);
				OutTargets.Add(MoveTemp(Target));
			}
		}

		const FString RepositoryRoot = OutTargets[0].RepositoryRoot;
		for (const FResolvedTarget& Target : OutTargets)
		{
			if (!Target.RepositoryRoot.Equals(RepositoryRoot, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("One scripted operation cannot mutate assets from multiple Git repositories.");
				return false;
			}
		}
		return true;
	}

	void SetWorkerPhase(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState,
		const EGitLocalSourceControlOperationPhase InPhase, const bool bInCancellationAllowed)
	{
		FScopeLock Lock(&InState->Mutex);
		InState->Phase = InPhase;
		InState->bCancellationAllowed = bInCancellationAllowed;
	}

	bool CanEnterMutation(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		FScopeLock Lock(&InState->Mutex);
		if (InState->CancellationContext->IsCancellationRequested())
		{
			return false;
		}
		InState->Phase = EGitLocalSourceControlOperationPhase::Mutating;
		InState->bCancellationAllowed = false;
		return true;
	}

	bool WasCancelled(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		return InState->CancellationContext->IsCancellationRequested();
	}

	void FinishWorker(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, FGitLocalSourceControlOperationResult&& InResult)
	{
		const bool bCancelled = InResult.bCancelled || WasCancelled(InState);
		InResult.bCancelled = bCancelled;
		if (bCancelled)
		{
			InResult.bSucceeded = false;
		}

		{
			FScopeLock Lock(&InState->Mutex);
			InResult.Errors.Append(InState->GuardErrors);
			InState->Result = MoveTemp(InResult);
			InState->Phase = bCancelled
				? EGitLocalSourceControlOperationPhase::Cancelled
				: InState->Result.bSucceeded ? EGitLocalSourceControlOperationPhase::Completed : EGitLocalSourceControlOperationPhase::Failed;
			InState->bCancellationAllowed = false;
			InState->bWorkerFinished = true;
		}
		GetOperationRegistry().End(InState);
	}

	void AddErrors(FGitLocalSourceControlOperationResult& InOutResult, const TArray<FString>& InErrors)
	{
		for (const FString& Error : InErrors)
		{
			if (!Error.IsEmpty())
			{
				InOutResult.Errors.Add(Error);
			}
		}
	}

	void AddAssetOperationResult(FGitLocalSourceControlOperationResult& InOutResult, const GitSourceControlAssetOperations::FGitAssetOperationResult& InAssetResult)
	{
		InOutResult.bSucceeded = InAssetResult.bSucceeded;
		InOutResult.bCancelled = InAssetResult.bCancelled;
		InOutResult.bReloadSucceeded = InAssetResult.bReloadSucceeded;
		InOutResult.AffectedFiles = InAssetResult.AffectedFiles;
		InOutResult.Errors = InAssetResult.Errors;
	}

	void AddHistory(FGitLocalSourceControlOperationResult& InOutResult, const TGitSourceControlHistory& InHistory)
	{
		InOutResult.History.Reserve(InHistory.Num());
		for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : InHistory)
		{
			FGitLocalSourceControlHistoryEntry& Entry = InOutResult.History.AddDefaulted_GetRef();
			Entry.CommitId = Revision->CommitId;
			Entry.HistoricalPath = Revision->Filename;
			Entry.Description = Revision->Description;
			Entry.Author = Revision->UserName;
			Entry.Action = Revision->Action;
			Entry.DateUtc = Revision->Date.ToIso8601();
		}
	}

	bool FindSelectedRevision(const TGitSourceControlHistory& InHistory, const FString& InSelector, FGitSourceControlRevision& OutRevision, FString& OutError)
	{
		FString Selector = InSelector;
		Selector.TrimStartAndEndInline();
		if (Selector.IsEmpty())
		{
			OutError = TEXT("A history revision id is required.");
			return false;
		}

		const FGitSourceControlRevision* Match = nullptr;
		for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Candidate : InHistory)
		{
			if (!Candidate->CommitId.StartsWith(Selector, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (Match != nullptr && !Match->CommitId.Equals(Candidate->CommitId, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("The supplied revision prefix is ambiguous. Use a longer commit id.");
				return false;
			}
			Match = &Candidate.Get();
		}
		if (Match == nullptr)
		{
			OutError = TEXT("The selected history revision is no longer available. Reload history and retry.");
			return false;
		}
		OutRevision = *Match;
		return true;
	}

	UGitLocalSourceControlOperation* MakeOperation(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState)
	{
		check(IsInGameThread());
		UGitLocalSourceControlOperation* Operation = NewObject<UGitLocalSourceControlOperation>(GetTransientPackage());
		Operation->Initialize(MoveTemp(InState));
		return Operation;
	}

	UGitLocalSourceControlOperation* MakeFailedOperation(const FString& InError)
	{
		TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
		FGitLocalSourceControlOperationResult Result;
		Result.Errors.Add(InError);
		FinishWorker(State, MoveTemp(Result));
		UGitLocalSourceControlOperation* Operation = MakeOperation(State);
		Operation->Tick();
		return Operation;
	}

	void AddGuardError(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, const FString& InError)
	{
		FScopeLock Lock(&InState->Mutex);
		InState->GuardErrors.Add(InError);
	}

	bool RevalidateClosedPackagesOnGameThread(const TArray<FString>& InAffectedFiles)
	{
		check(IsInGameThread());
		for (const FString& Filename : InAffectedFiles)
		{
			FString PackageName;
			if (!FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				continue;
			}
			if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
			{
				return false;
			}
		}
		return true;
	}

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakePreauthorizedClosedAssetCallbacks(
		const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		GitSourceControlAssetOperations::FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [InState](const FString&, const TArray<FString>&)
		{
			return InvokeOnGameThreadAndWait(InState, []()
			{
				return true;
			});
		};
		Callbacks.PrepareForMutation = [InState](const TArray<FString>& AffectedFiles)
		{
			const bool bClosed = InvokeOnGameThreadAndWait(InState, [AffectedFiles]()
			{
				return RevalidateClosedPackagesOnGameThread(AffectedFiles);
			});
			if (!bClosed)
			{
				AddGuardError(InState, TEXT("The asset package was loaded while the operation was pending. No workspace file was changed."));
				return false;
			}
			return CanEnterMutation(InState);
		};
		Callbacks.ReloadPackages = [InState](const TArray<FString>&)
		{
			// Start-time validation rejects all loaded packages. Keep callbacks on this
			// plain worker state path rather than touching UObject or Editor services.
			SetWorkerPhase(InState, EGitLocalSourceControlOperationPhase::Refreshing, false);
			return InvokeOnGameThreadAndWait(InState, []()
			{
				return true;
			});
		};
		return Callbacks;
	}
}

void UGitLocalSourceControlOperation::Initialize(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState)
{
	check(IsInGameThread());
	State = MoveTemp(InState);
}

void UGitLocalSourceControlOperation::Tick()
{
	check(IsInGameThread());
	if (!State.IsValid() || bTerminal)
	{
		return;
	}

	FScopeLock Lock(&State->Mutex);
	if (!State->bWorkerFinished)
	{
		return;
	}
	Result = State->Result;
	CompletedPhase = State->Phase;
	bTerminal = true;
}

bool UGitLocalSourceControlOperation::Cancel()
{
	check(IsInGameThread());
	if (!State.IsValid() || bTerminal)
	{
		return false;
	}
	return State->RequestShutdownCancellationIfAllowed();
}

bool UGitLocalSourceControlOperation::IsTerminal() const
{
	check(IsInGameThread());
	return bTerminal;
}

EGitLocalSourceControlOperationPhase UGitLocalSourceControlOperation::GetPhase() const
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return EGitLocalSourceControlOperationPhase::Failed;
	}
	if (bTerminal)
	{
		return CompletedPhase;
	}
	FScopeLock Lock(&State->Mutex);
	return State->Phase;
}

FGitLocalSourceControlOperationResult UGitLocalSourceControlOperation::GetResult() const
{
	check(IsInGameThread());
	return Result;
}

FGitLocalSourceControlProviderInfo UGitLocalSourceControlLibrary::GetProviderInfo()
{
	return GitLocalSourceControlPrivate::GetProviderInfoOnGameThread();
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartLoadHistory(const FString& AssetObjectPath)
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::FResolvedTarget Target;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTarget(AssetObjectPath, GitLocalSourceControlPrivate::ETargetAccess::ReadOnly, Target, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Target]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		if (GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false, Errors, History))
		{
			Result.bSucceeded = true;
			GitLocalSourceControlPrivate::AddHistory(Result, History);
		}
		else
		{
			GitLocalSourceControlPrivate::AddErrors(Result, Errors);
		}
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartFetchLfsRevision(const FString& AssetObjectPath, const FString& Revision)
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::FResolvedTarget Target;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTarget(AssetObjectPath, GitLocalSourceControlPrivate::ETargetAccess::ReadOnly, Target, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		if (!GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false, Errors, History))
		{
			GitLocalSourceControlPrivate::AddErrors(Result, Errors);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		FGitSourceControlRevision SelectedRevision;
		FString SelectionError;
		if (!GitLocalSourceControlPrivate::FindSelectedRevision(History, Revision, SelectedRevision, SelectionError))
		{
			Result.Errors.Add(SelectionError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::FetchingLfs, true);
		FString FetchError;
		Result.bSucceeded = GitSourceControlUtils::FetchLfsContentForRevision(Target.GitBinary, Target.RepositoryRoot, SelectedRevision.CommitId, SelectedRevision.Filename, FetchError);
		if (!Result.bSucceeded && !FetchError.IsEmpty())
		{
			Result.Errors.Add(FetchError);
		}
		Result.AffectedFiles.Add(Target.Filename);
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartRestoreRevision(const FString& AssetObjectPath, const FString& Revision)
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::FResolvedTarget Target;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTarget(AssetObjectPath, GitLocalSourceControlPrivate::ETargetAccess::Mutation, Target, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		if (!GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false, Errors, History))
		{
			GitLocalSourceControlPrivate::AddErrors(Result, Errors);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		FGitSourceControlRevision SelectedRevision;
		FString SelectionError;
		if (!GitLocalSourceControlPrivate::FindSelectedRevision(History, Revision, SelectedRevision, SelectionError))
		{
			Result.Errors.Add(SelectionError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}

		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::Preparing, true);
		GitLocalSourceControlPrivate::FRepositoryMutationGuard MutationGuard(Target.RepositoryRoot, State);
		if (!MutationGuard.Acquire())
		{
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		GitSourceControlAssetOperations::FGitSourceControlAssetOperations Operations(Target.GitBinary, Target.RepositoryRoot);
		GitSourceControlAssetOperations::FGitAssetOperationResult AssetResult;
		const bool bRestored = Operations.RestoreRevisionToWorkspace(Target.Filename, SelectedRevision.CommitId, SelectedRevision.Filename,
			GitLocalSourceControlPrivate::MakePreauthorizedClosedAssetCallbacks(State), AssetResult);
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bRestored && AssetResult.bSucceeded;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartRefreshStatus(const TArray<FString>& AssetObjectPaths)
{
	check(IsInGameThread());
	TArray<GitLocalSourceControlPrivate::FResolvedTarget> Targets;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTargets(AssetObjectPaths, GitLocalSourceControlPrivate::ETargetAccess::ReadOnly, Targets, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Targets]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::Refreshing, true);
		FGitLocalSourceControlOperationResult Result;
		TArray<FString> Files;
		for (const GitLocalSourceControlPrivate::FResolvedTarget& Target : Targets)
		{
			Files.Add(Target.Filename);
		}
		TArray<FString> Errors;
		TMap<FString, FGitSourceControlState> States;
		Result.bSucceeded = GitSourceControlUtils::RunUpdateStatus(Targets[0].GitBinary, Targets[0].RepositoryRoot, false, Files, Errors, States);
		Result.AffectedFiles = MoveTemp(Files);
		GitLocalSourceControlPrivate::AddErrors(Result, Errors);
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartDiscardTracked(const TArray<FString>& AssetObjectPaths)
{
	check(IsInGameThread());
	TArray<GitLocalSourceControlPrivate::FResolvedTarget> Targets;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTargets(AssetObjectPaths, GitLocalSourceControlPrivate::ETargetAccess::Mutation, Targets, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Targets]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::Preparing, true);
		TArray<FString> Files;
		for (const GitLocalSourceControlPrivate::FResolvedTarget& Target : Targets)
		{
			Files.Add(Target.Filename);
		}
		GitLocalSourceControlPrivate::FRepositoryMutationGuard MutationGuard(Targets[0].RepositoryRoot, State);
		if (!MutationGuard.Acquire())
		{
			GitLocalSourceControlPrivate::FinishWorker(State, FGitLocalSourceControlOperationResult());
			return;
		}
		GitSourceControlAssetOperations::FGitSourceControlAssetOperations Operations(Targets[0].GitBinary, Targets[0].RepositoryRoot);
		GitSourceControlAssetOperations::FGitAssetOperationResult AssetResult;
		const bool bDiscarded = Operations.DiscardTrackedFiles(Files, GitLocalSourceControlPrivate::MakePreauthorizedClosedAssetCallbacks(State), AssetResult);
		FGitLocalSourceControlOperationResult Result;
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bDiscarded && AssetResult.bSucceeded;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartDeleteUntracked(const TArray<FString>& AssetObjectPaths)
{
	check(IsInGameThread());
	TArray<GitLocalSourceControlPrivate::FResolvedTarget> Targets;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTargets(AssetObjectPaths, GitLocalSourceControlPrivate::ETargetAccess::Mutation, Targets, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Targets]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::Preparing, true);
		TArray<FString> Files;
		for (const GitLocalSourceControlPrivate::FResolvedTarget& Target : Targets)
		{
			Files.Add(Target.Filename);
		}
		GitLocalSourceControlPrivate::FRepositoryMutationGuard MutationGuard(Targets[0].RepositoryRoot, State);
		if (!MutationGuard.Acquire())
		{
			GitLocalSourceControlPrivate::FinishWorker(State, FGitLocalSourceControlOperationResult());
			return;
		}
		GitSourceControlAssetOperations::FGitSourceControlAssetOperations Operations(Targets[0].GitBinary, Targets[0].RepositoryRoot);
		GitSourceControlAssetOperations::FGitAssetOperationResult AssetResult;
		const bool bDeleted = Operations.DeleteUntrackedFiles(Files, GitLocalSourceControlPrivate::MakePreauthorizedClosedAssetCallbacks(State), AssetResult);
		FGitLocalSourceControlOperationResult Result;
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bDeleted && AssetResult.bSucceeded;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

void GitLocalSourceControl::ShutdownOperations()
{
	GitLocalSourceControlPrivate::GetOperationRegistry().Shutdown();
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLocalSourceControlShutdownMutationAutomationTest,
	"Cthulhu.GitSourceControl.Api.ShutdownMutationCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitLocalSourceControlShutdownMutationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FGitLocalSourceControlOperationState MutationState;
	MutationState.bCancellationAllowed = false;
	TestFalse(TEXT("Shutdown does not cancel an operation after it enters mutation"), MutationState.RequestShutdownCancellationIfAllowed());
	TestFalse(TEXT("Mutation cancellation token remains clear during shutdown"), MutationState.CancellationContext->IsCancellationRequested());

	FGitLocalSourceControlOperationState ReadOnlyState;
	TestTrue(TEXT("Shutdown cancels a pre-mutation operation"), ReadOnlyState.RequestShutdownCancellationIfAllowed());
	return TestTrue(TEXT("Read-only cancellation token is set during shutdown"), ReadOnlyState.CancellationContext->IsCancellationRequested());
}

#endif
