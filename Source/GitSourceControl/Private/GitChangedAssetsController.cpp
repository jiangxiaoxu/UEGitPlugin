// Copyright (c) 2026

#include "GitChangedAssetsController.h"

#include "GitChangedAssetOperations.h"
#include "GitChangedAssetsMetadata.h"
#include "GitChangedAssetsStatus.h"
#include "GitSourceControlUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitChangedAssetsControllerPrivate
{
	class FGameThreadDispatcher final : public TSharedFromThis<FGameThreadDispatcher, ESPMode::ThreadSafe>
	{
	public:
		class FRequest final
		{
		public:
			explicit FRequest(TFunction<bool()>&& InWork)
				: Work(MoveTemp(InWork))
				, CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
			{
			}

			~FRequest()
			{
				FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
			}

			void Complete(const bool bInResult)
			{
				FScopeLock Lock(&Mutex);
				if (!bCompleted)
				{
					bResult = bInResult;
					bCompleted = true;
					CompletionEvent->Trigger();
				}
			}

			bool WaitForResult()
			{
				CompletionEvent->Wait();
				FScopeLock Lock(&Mutex);
				return bResult;
			}

			bool Execute()
			{
				return Work ? Work() : false;
			}

		private:
			FCriticalSection Mutex;
			TFunction<bool()> Work;
			FEvent* CompletionEvent = nullptr;
			bool bCompleted = false;
			bool bResult = false;
		};

		void Start()
		{
			check(IsInGameThread());
		}

		bool InvokeAndWait(TFunction<bool()>&& Work)
		{
			if (IsInGameThread())
			{
				return Work ? Work() : false;
			}
			const TSharedRef<FRequest, ESPMode::ThreadSafe> Request = MakeShared<FRequest, ESPMode::ThreadSafe>(MoveTemp(Work));
			{
				FScopeLock Lock(&Mutex);
				if (!bAcceptingRequests)
				{
					Request->Complete(false);
				}
				else
				{
					PendingRequests.Add(Request);
					EnsureTickerLocked();
				}
			}
			return Request->WaitForResult();
		}

		bool Post(TFunction<bool()>&& Work)
		{
			const TSharedRef<FRequest, ESPMode::ThreadSafe> Request = MakeShared<FRequest, ESPMode::ThreadSafe>(MoveTemp(Work));
			FScopeLock Lock(&Mutex);
			if (!bAcceptingRequests)
			{
				return false;
			}
			PendingRequests.Add(Request);
			EnsureTickerLocked();
			return true;
		}

		void Pump()
		{
			check(IsInGameThread());
			TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> Requests;
			{
				FScopeLock Lock(&Mutex);
				Requests = MoveTemp(PendingRequests);
			}
			for (const TSharedRef<FRequest, ESPMode::ThreadSafe>& Request : Requests)
			{
				Request->Complete(Request->Execute());
			}
		}

		void Close()
		{
			check(IsInGameThread());
			TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> RequestsToCancel;
			{
				FScopeLock Lock(&Mutex);
				if (!bAcceptingRequests && !TickerHandle.IsValid())
				{
					return;
				}
				bAcceptingRequests = false;
				RequestsToCancel = MoveTemp(PendingRequests);
				if (TickerHandle.IsValid())
				{
					FTSTicker::RemoveTicker(TickerHandle);
					TickerHandle.Reset();
				}
			}
			for (const TSharedRef<FRequest, ESPMode::ThreadSafe>& Request : RequestsToCancel)
			{
				Request->Complete(false);
			}
		}

	private:
		bool Tick(const float DeltaTime)
		{
			(void)DeltaTime;
			Pump();
			FScopeLock Lock(&Mutex);
			if (!bAcceptingRequests || PendingRequests.IsEmpty())
			{
				TickerHandle.Reset();
				return false;
			}
			return true;
		}

		void EnsureTickerLocked()
		{
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(AsShared(), &FGameThreadDispatcher::Tick), 0.0f);
			}
		}

		FCriticalSection Mutex;
		TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> PendingRequests;
		FTSTicker::FDelegateHandle TickerHandle;
		bool bAcceptingRequests = true;
	};

	class FWorkerState final : public TSharedFromThis<FWorkerState, ESPMode::ThreadSafe>
	{
	public:
		FWorkerState()
			: AllWorkersCompletedEvent(FPlatformProcess::GetSynchEventFromPool(true))
		{
			AllWorkersCompletedEvent->Trigger();
		}

		~FWorkerState()
		{
			FPlatformProcess::ReturnSynchEventToPool(AllWorkersCompletedEvent);
		}

		bool TryBegin(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& CancellationContext,
			const bool bCancellable)
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingWorkers)
			{
				return false;
			}
			++ActiveWorkerCount;
			FTrackedWorker& TrackedWorker = TrackedWorkers.AddDefaulted_GetRef();
			TrackedWorker.Context = CancellationContext;
			TrackedWorker.bCancellable = bCancellable;
			AllWorkersCompletedEvent->Reset();
			return true;
		}

		void End(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& CancellationContext)
		{
			FScopeLock Lock(&Mutex);
			check(ActiveWorkerCount > 0);
			TrackedWorkers.RemoveAllSwap([&CancellationContext](const FTrackedWorker& Existing)
			{
				const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> PinnedContext = Existing.Context.Pin();
				return !PinnedContext.IsValid() || PinnedContext.Get() == &CancellationContext.Get();
			}, EAllowShrinking::No);
			if (--ActiveWorkerCount == 0)
			{
				AllWorkersCompletedEvent->Trigger();
			}
		}

		void EnterCommitPhase(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& CancellationContext)
		{
			FScopeLock Lock(&Mutex);
			for (FTrackedWorker& Existing : TrackedWorkers)
			{
				if (const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> PinnedContext = Existing.Context.Pin();
					PinnedContext.IsValid() && PinnedContext.Get() == &CancellationContext.Get())
				{
					Existing.bCancellable = false;
					return;
				}
			}
		}

		void CancelCancellableWorkers()
		{
			TArray<TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>> ContextsToCancel;
			{
				FScopeLock Lock(&Mutex);
				for (const FTrackedWorker& Existing : TrackedWorkers)
				{
					if (Existing.bCancellable)
					{
						if (TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> PinnedContext = Existing.Context.Pin())
						{
							ContextsToCancel.Add(MoveTemp(PinnedContext));
						}
					}
				}
			}
			for (const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& Context : ContextsToCancel)
			{
				Context->Cancel();
			}
		}

		void StopAccepting()
		{
			FScopeLock Lock(&Mutex);
			bAcceptingWorkers = false;
		}

		bool HasActiveWorkers() const
		{
			FScopeLock Lock(&Mutex);
			return ActiveWorkerCount != 0;
		}

	private:
		struct FTrackedWorker
		{
			TWeakPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> Context;
			bool bCancellable = true;
		};

		mutable FCriticalSection Mutex;
		FEvent* AllWorkersCompletedEvent = nullptr;
		int32 ActiveWorkerCount = 0;
		bool bAcceptingWorkers = true;
		TArray<FTrackedWorker> TrackedWorkers;
	};

	class FScopedWorker final
	{
	public:
		FScopedWorker(const TSharedRef<FWorkerState, ESPMode::ThreadSafe>& InWorkerState,
			const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext)
			: WorkerState(InWorkerState)
			, CancellationContext(InCancellationContext)
		{
		}

		~FScopedWorker()
		{
			WorkerState->End(CancellationContext);
		}

	private:
		TSharedRef<FWorkerState, ESPMode::ThreadSafe> WorkerState;
		TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
	};

	bool InvokeOnGameThreadAndWait(const TSharedRef<FGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher, TFunction<bool()>&& Work)
	{
		return Dispatcher->InvokeAndWait(MoveTemp(Work));
	}

	FString MakeRevertResultMessage(const GitChangedAssetOperations::FGitChangedAssetRevertResult& Result)
	{
		if (Result.bSucceeded)
		{
			if (!Result.bReloadSucceeded)
			{
				const FString Diagnostics = Result.Errors.IsEmpty() ? TEXT("no additional Editor diagnostic was returned") : FString::Join(Result.Errors, TEXT("\n"));
				return FString::Printf(TEXT("Git files were reverted, but the Editor reload or refresh reported a failure:\n%s"), *Diagnostics);
			}
			return FString::Printf(TEXT("Reverted %d .uasset file(s) to HEAD."), Result.AffectedFiles.Num());
		}
		if (Result.bCancelled)
		{
			return TEXT("Revert was cancelled before changing Git files.");
		}
		return Result.Errors.IsEmpty() ? TEXT("Revert failed without a diagnostic.") : FString::Join(Result.Errors, TEXT("\n"));
	}
}

FGitChangedAssetsController::FGitChangedAssetsController()
{
	check(IsInGameThread());
	GameThreadDispatcher = MakeShared<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe>();
	GameThreadDispatcher->Start();
	WorkerState = MakeShared<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe>();
}

FGitChangedAssetsController::~FGitChangedAssetsController() = default;

void FGitChangedAssetsController::Refresh(const bool bClearPreviousError)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || bRefreshing || bReverting)
	{
		return;
	}

	bRefreshing = true;
	bPreserveLastErrorForRefresh = !bClearPreviousError;
	if (bClearPreviousError)
	{
		LastError.Empty();
	}
	const uint64 RequestedGeneration = ++Generation;
	const FString ProjectFile = FPaths::GetProjectFilePath();
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	const TSharedRef<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> RefreshWorkerState = WorkerState.ToSharedRef();
	const TSharedRef<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = GameThreadDispatcher.ToSharedRef();
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	if (!RefreshWorkerState->TryBegin(CancellationContext, true))
	{
		bRefreshing = false;
		LastError = TEXT("Changed Assets is shutting down and cannot start a refresh.");
		ChangedDelegate.Broadcast();
		return;
	}
	ChangedDelegate.Broadcast();

	Async(EAsyncExecution::ThreadPool, [WeakController, RequestedGeneration, ProjectFile, RefreshWorkerState, Dispatcher, CancellationContext]()
	{
		GitChangedAssetsControllerPrivate::FScopedWorker Worker(RefreshWorkerState, CancellationContext);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		FString GitBinary;
		FString RepositoryRoot;
		FString Error;
		TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot;
		if (GitSourceControlUtils::ResolveStandaloneRepositoryForFile(ProjectFile, GitBinary, RepositoryRoot, Error))
		{
			FGitChangedAssetSnapshot SnapshotResult;
			if (FGitChangedAssetsStatus::CaptureSnapshot(GitBinary, RepositoryRoot, RequestedGeneration, SnapshotResult, Error))
			{
				CompletedSnapshot = MakeShared<FGitChangedAssetSnapshot, ESPMode::ThreadSafe>(MoveTemp(SnapshotResult));
			}
		}

		GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, RequestedGeneration, CompletedSnapshot = MoveTemp(CompletedSnapshot), Error = MoveTemp(Error)]() mutable
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteRefresh(RequestedGeneration, MoveTemp(CompletedSnapshot), MoveTemp(Error));
			}
			return true;
		});
	});
}

void FGitChangedAssetsController::Shutdown()
{
	check(IsInGameThread());
	if (bShuttingDown.Exchange(true))
	{
		return;
	}
	++Generation;
	WorkerState->StopAccepting();
	WorkerState->CancelCancellableWorkers();
	while (WorkerState->HasActiveWorkers())
	{
		// Workers can be blocked in Prepare/Finalize or completion delivery. Pumping on
		// the GameThread lets their providerless transaction reach a safe terminal state.
		GameThreadDispatcher->Pump();
		FPlatformProcess::SleepNoStats(0.001f);
	}
	GameThreadDispatcher->Close();
	bRefreshing = false;
	bReverting = false;
	ChangedDelegate.Clear();
	Snapshot.Reset();
}

bool FGitChangedAssetsController::IsRefreshing() const
{
	return bRefreshing;
}

bool FGitChangedAssetsController::IsReverting() const
{
	return bReverting;
}

const FGitChangedAssetSnapshot* FGitChangedAssetsController::GetSnapshot() const
{
	return Snapshot.IsSet() ? &Snapshot.GetValue() : nullptr;
}

const FString& FGitChangedAssetsController::GetLastError() const
{
	return LastError;
}

const FDateTime& FGitChangedAssetsController::GetLastSuccessfulRefreshTime() const
{
	return LastSuccessfulRefreshTime;
}

void FGitChangedAssetsController::RevertToHead(TArray<FGitChangedAssetEntry> Entries)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || bRefreshing || bReverting || !Snapshot.IsSet() || Entries.IsEmpty())
	{
		return;
	}

	FString EligibilityError;
	if (!GitChangedAssetOperations::FGitChangedAssetOperations::ValidateEntries(Entries, EligibilityError))
	{
		LastError = MoveTemp(EligibilityError);
		ChangedDelegate.Broadcast();
		return;
	}

	bReverting = true;
	LastError.Empty();
	const FString GitBinary = Snapshot->GitBinary;
	const FString RepositoryRoot = Snapshot->RepositoryRoot;
	const FString PinnedHead = Snapshot->PinnedHead;
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	const TSharedRef<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> RevertWorkerState = WorkerState.ToSharedRef();
	const TSharedRef<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = GameThreadDispatcher.ToSharedRef();
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	if (!RevertWorkerState->TryBegin(CancellationContext, true))
	{
		bReverting = false;
		LastError = TEXT("Changed Assets is shutting down and cannot start a revert.");
		ChangedDelegate.Broadcast();
		return;
	}
	ChangedDelegate.Broadcast();

	Async(EAsyncExecution::ThreadPool, [WeakController, GitBinary, RepositoryRoot, PinnedHead, Entries = MoveTemp(Entries), RevertWorkerState, Dispatcher, CancellationContext]() mutable
	{
		GitChangedAssetsControllerPrivate::FScopedWorker Worker(RevertWorkerState, CancellationContext);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		const TSharedRef<GitChangedAssetOperations::FGitChangedAssetRevertLifecycle, ESPMode::ThreadSafe> Lifecycle =
			MakeShared<GitChangedAssetOperations::FGitChangedAssetRevertLifecycle, ESPMode::ThreadSafe>();
		GitChangedAssetOperations::FGitChangedAssetRevertCallbacks Callbacks;
		Callbacks.IsCancellationRequested = [CancellationContext]()
		{
			return CancellationContext->IsCancellationRequested();
		};
		Callbacks.Confirm = [WeakController, Dispatcher, Lifecycle](const TArray<FGitChangedAssetEntry>& CallbackEntries, FString& OutError)
		{
			return GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, Lifecycle, &CallbackEntries, &OutError]()
			{
				if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
				{
					return Controller->ConfirmRevert(CallbackEntries, OutError) && Lifecycle->RecordConfirmedClosure(CallbackEntries, OutError);
				}
				OutError = TEXT("Changed Assets closed before Revert confirmation.");
				return false;
			});
		};
		Callbacks.PrepareForMutation = [Lifecycle, Dispatcher](const TArray<FGitChangedAssetEntry>& CallbackEntries, FString& OutError)
		{
			return GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [Lifecycle, &CallbackEntries, &OutError]()
			{
				return Lifecycle->Prepare(CallbackEntries, OutError);
			});
		};
		Callbacks.BeginMutation = [Lifecycle, Dispatcher, RevertWorkerState, CancellationContext](const TArray<FGitChangedAssetEntry>& CallbackEntries, FString& OutError)
		{
			return GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [Lifecycle, RevertWorkerState, CancellationContext, &CallbackEntries, &OutError]()
			{
				const bool bReadyToCommit = Lifecycle->BeginMutation(CallbackEntries, OutError);
				if (bReadyToCommit)
				{
					RevertWorkerState->EnterCommitPhase(CancellationContext);
				}
				return bReadyToCommit;
			});
		};
		Callbacks.FinalizeEditor = [Lifecycle, Dispatcher](const TArray<FGitChangedAssetEntry>& CallbackEntries, const TArray<FString>& AffectedFiles,
			const GitChangedAssetOperations::EGitChangedAssetMutationOutcome Outcome, FString& OutError)
		{
			return GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [Lifecycle, &CallbackEntries, &AffectedFiles, Outcome, &OutError]()
			{
				const bool bFinished = Lifecycle->Finish(CallbackEntries, AffectedFiles, Outcome, OutError);
				if (Outcome == GitChangedAssetOperations::EGitChangedAssetMutationOutcome::Succeeded)
				{
					IAssetRegistry::GetChecked().ScanModifiedAssetFiles(AffectedFiles);
					FEditorDelegates::RefreshAllBrowsers.Broadcast();
				}
				return bFinished;
			});
		};

		GitChangedAssetOperations::FGitChangedAssetRevertResult Result;
		const bool bSucceeded = GitChangedAssetOperations::FGitChangedAssetOperations(GitBinary, RepositoryRoot)
			.RevertToHead(PinnedHead, Entries, Callbacks, Result);
		const bool bEditorReloadSucceeded = Result.bReloadSucceeded;
		FString ResultMessage = GitChangedAssetsControllerPrivate::MakeRevertResultMessage(Result);
		GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, bSucceeded, bEditorReloadSucceeded, ResultMessage = MoveTemp(ResultMessage)]() mutable
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteRevert(bSucceeded, bEditorReloadSucceeded, MoveTemp(ResultMessage));
			}
			return true;
		});
	});
}

FGitChangedAssetsController::FOnChangedAssetsUpdated& FGitChangedAssetsController::OnChanged()
{
	return ChangedDelegate;
}

void FGitChangedAssetsController::CompleteRefresh(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation)
	{
		return;
	}
	bRefreshing = false;
	const bool bPreserveLastError = bPreserveLastErrorForRefresh;
	bPreserveLastErrorForRefresh = false;
	if (!CompletedSnapshot.IsValid())
	{
		LastError = Error.IsEmpty() ? TEXT("Git Changes refresh failed without a diagnostic.") : MoveTemp(Error);
		ChangedDelegate.Broadcast();
		return;
	}

	LastSuccessfulRefreshTime = FDateTime::UtcNow();
	if (!bPreserveLastError)
	{
		LastError.Empty();
	}
	// Do not publish raw Git rows: untracked external packages have no usable package name until
	// the current-file resolver canonicalizes/scans their exact .uasset files.
	CompleteCurrentMetadata(CompletedGeneration, MoveTemp(CompletedSnapshot));
}

void FGitChangedAssetsController::CompleteCurrentMetadata(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}

	FGitChangedAssetsMetadataResolver::ResolveCurrentMetadata(*CompletedSnapshot);
	Snapshot = MoveTemp(*CompletedSnapshot);

	const bool bNeedsHeadMetadata = Snapshot->Entries.ContainsByPredicate([](const FGitChangedAssetEntry& Entry)
	{
		return Entry.State == EGitChangedAssetState::Deleted && !Entry.bMetadataResolved;
	});
	if (!bNeedsHeadMetadata)
	{
		ChangedDelegate.Broadcast();
		const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
		GameThreadDispatcher->Post([WeakController, CompletedGeneration]()
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteOwnerFallback(CompletedGeneration);
			}
			return true;
		});
		return;
	}
	ChangedDelegate.Broadcast();

	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> HeadMetadataSnapshot =
		MakeShared<FGitChangedAssetSnapshot, ESPMode::ThreadSafe>(Snapshot.GetValue());
	const TSharedRef<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> HeadMetadataWorkerState = WorkerState.ToSharedRef();
	const TSharedRef<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = GameThreadDispatcher.ToSharedRef();
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	if (!HeadMetadataWorkerState->TryBegin(CancellationContext, true))
	{
		return;
	}
	Async(EAsyncExecution::ThreadPool, [WeakController, CompletedGeneration, HeadMetadataSnapshot, HeadMetadataWorkerState, Dispatcher, CancellationContext]()
	{
		GitChangedAssetsControllerPrivate::FScopedWorker Worker(HeadMetadataWorkerState, CancellationContext);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		FString HeadMetadataError;
		TArray<FGitChangedAssetHeadMetadataResult> HeadMetadataResults;
		FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(*HeadMetadataSnapshot, HeadMetadataResults, HeadMetadataError, CancellationContext);
		GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, CompletedGeneration, HeadMetadataSnapshot,
			HeadMetadataResults = MoveTemp(HeadMetadataResults), HeadMetadataError = MoveTemp(HeadMetadataError)]() mutable
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteHeadMetadata(CompletedGeneration, HeadMetadataSnapshot, MoveTemp(HeadMetadataResults), MoveTemp(HeadMetadataError));
			}
			return true;
		});
	});
}

void FGitChangedAssetsController::CompleteHeadMetadata(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, TArray<FGitChangedAssetHeadMetadataResult> Results, FString Error)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}
	FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(*CompletedSnapshot, Results);
	Snapshot = MoveTemp(*CompletedSnapshot);
	if (!Error.IsEmpty())
	{
		LastError = LastError.IsEmpty() ? MoveTemp(Error) : LastError + TEXT("\n") + Error;
	}
	ChangedDelegate.Broadcast();
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	GameThreadDispatcher->Post([WeakController, CompletedGeneration]()
	{
		if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
		{
			Controller->CompleteOwnerFallback(CompletedGeneration);
		}
		return true;
	});
}

void FGitChangedAssetsController::CompleteOwnerFallback(const uint64 CompletedGeneration)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !Snapshot.IsSet())
	{
		return;
	}
	FGitChangedAssetsMetadataResolver::ResolveOutstandingOwnerFallback(Snapshot.GetValue());
	ChangedDelegate.Broadcast();
}

bool FGitChangedAssetsController::ConfirmRevert(const TArray<FGitChangedAssetEntry>& Entries, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (bShuttingDown.Load())
	{
		OutError = TEXT("Changed Assets is shutting down.");
		return false;
	}

	GitChangedAssetOperations::FGitChangedAssetRevertPreview Preview;
	if (!GitChangedAssetOperations::FGitChangedAssetRevertLifecycle::BuildPreview(Entries, Preview, OutError))
	{
		return false;
	}

	bool bDeletesFile = false;
	for (const FGitChangedAssetEntry& Entry : Entries)
	{
		bDeletesFile |= Entry.State == EGitChangedAssetState::Added || Entry.State == EGitChangedAssetState::Untracked;
	}

	FString Message = FString::Printf(TEXT("Revert %d selected asset(s) to HEAD?\n\nThis discards both staged and unstaged changes for exactly the selected .uasset files. This operation has no Undo. Missing HEAD Git LFS objects may be fetched for the exact selected paths after confirmation."), Entries.Num());
	if (bDeletesFile)
	{
		Message += TEXT("\n\nAdded or untracked selected assets will be deleted.");
	}
	if (!Preview.SelectedDirtyPackageNames.IsEmpty())
	{
		Message += TEXT("\n\nSelected dirty packages whose unsaved changes will be discarded:\n") + FString::Join(Preview.SelectedDirtyPackageNames, TEXT("\n"));
	}
	if (!Preview.OwnerMapsToReload.IsEmpty())
	{
		Message += TEXT("\n\nOwner level(s) to reload:\n") + FString::Join(Preview.OwnerMapsToReload, TEXT("\n"));
	}

	if (FMessageDialog::Open(EAppMsgType::OkCancel, FText::FromString(Message), LOCTEXT("ConfirmChangedAssetsRevertTitle", "Revert Git Changes")) != EAppReturnType::Ok)
	{
		return false;
	}
	return true;
}

void FGitChangedAssetsController::CompleteRevert(const bool bDiskMutationSucceeded, const bool bEditorReloadSucceeded, FString ResultMessage)
{
	check(IsInGameThread());
	if (bShuttingDown.Load())
	{
		return;
	}
	bReverting = false;
	if (!bDiskMutationSucceeded)
	{
		LastError = MoveTemp(ResultMessage);
		ChangedDelegate.Broadcast();
		return;
	}

	if (!bEditorReloadSucceeded)
	{
		LastError = MoveTemp(ResultMessage);
		ChangedDelegate.Broadcast();
		Refresh(false);
		return;
	}

	LastError.Empty();
	ChangedDelegate.Broadcast();
	Refresh();
}

#undef LOCTEXT_NAMESPACE
