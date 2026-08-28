// Copyright (c) 2026

#include "GitChangedAssetsController.h"

#include "GitChangedAssetOperations.h"
#include "GitChangedAssetsMetadata.h"
#include "GitChangedAssetsStatus.h"
#include "GitSourceControlUtils.h"
#include "GitStandaloneLog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitChangedAssetsControllerPrivate
{
	constexpr int32 HeadMetadataApplyMaxResultsPerTick = 8;
	constexpr double HeadMetadataApplyTimeBudgetSeconds = 0.003;
	constexpr int32 CurrentMetadataApplyMaxEntriesPerTick = 8;
	constexpr double CurrentMetadataApplyTimeBudgetSeconds = 0.003;

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

	void EmitRevertTelemetry(const TSharedPtr<GitChangedAssetOperations::FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe>& InTelemetry)
	{
		if (!InTelemetry.IsValid())
		{
			return;
		}

		const TCHAR* const Terminal = !InTelemetry->bDiskMutationSucceeded
			? (InTelemetry->bCancelled ? TEXT("cancelled") : TEXT("disk_failed"))
			: !InTelemetry->bEditorReloadSucceeded
				? (InTelemetry->bPostRefreshAttempted && !InTelemetry->bPostRefreshSucceeded ? TEXT("reload_and_refresh_failed") : TEXT("reload_failed"))
				: InTelemetry->bPostRefreshAttempted && !InTelemetry->bPostRefreshSucceeded ? TEXT("post_refresh_failed") : TEXT("completed");

		UE_LOG(LogGitStandalone, Verbose, TEXT("Changed Assets Revert timing: terminal=%s selected=%d planned=%d uniqueLfsOids=%d lfsPointerReads=%d lfsPointers=%d lfsVerifies=%d lfsFetches=%d fingerprintBytes=%lld backupBytes=%lld gitHeadChecks=%d gitStatusChecks=%d gitIndexSnapshots=%d gitBlobReads=%d gitRestoreBatches=%d gitResetBatches=%d gitIndexRollbacks=%d diskMutation=%d editorReload=%d postRefreshAttempted=%d postRefreshSucceeded=%d preflight=%.3fs confirm=%.3fs lfs=%.3fs prepare=%.3fs loaderReset=%.3fs diskMutationTime=%.3fs editorFinalize=%.3fs assetRegistry=%.3fs browserRefresh=%.3fs postRefreshStatus=%.3fs postRefreshMetadata=%.3fs postRefreshTotal=%.3fs"),
			Terminal, InTelemetry->SelectedEntryCount, InTelemetry->PlannedFileCount, InTelemetry->UniqueLfsObjectCount,
			InTelemetry->LfsPointerReadCount, InTelemetry->LfsPointerCount, InTelemetry->LfsVerifyCount, InTelemetry->LfsFetchCount,
			InTelemetry->FingerprintBytes, InTelemetry->BackupBytes, InTelemetry->GitHeadCheckCount, InTelemetry->GitStatusCheckCount,
			InTelemetry->GitIndexSnapshotCount, InTelemetry->GitBlobReadCount, InTelemetry->GitRestoreBatchCount, InTelemetry->GitResetBatchCount,
			InTelemetry->GitIndexRollbackCount, InTelemetry->bDiskMutationSucceeded, InTelemetry->bEditorReloadSucceeded,
			InTelemetry->bPostRefreshAttempted, InTelemetry->bPostRefreshSucceeded, InTelemetry->PreflightSeconds, InTelemetry->ConfirmationSeconds,
			InTelemetry->LfsSeconds, InTelemetry->PrepareSeconds, InTelemetry->LoaderResetSeconds, InTelemetry->DiskMutationSeconds,
			InTelemetry->EditorFinalizeSeconds, InTelemetry->AssetRegistrySeconds, InTelemetry->BrowserRefreshSeconds,
			InTelemetry->PostRefreshStatusSeconds, InTelemetry->PostRefreshMetadataSeconds, InTelemetry->PostRefreshTotalSeconds);
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
	if (!GitSourceControlUtils::IsStartupGitCapabilityAvailable())
	{
		LastError = GitSourceControlUtils::GetStartupGitCapabilityMessage().ToString();
		ActivityChangedDelegate.Broadcast();
		return;
	}

	ClearPendingRefreshWork();
	bRefreshing = true;
	bPreserveLastErrorForRefresh = !bClearPreviousError;
	if (bClearPreviousError)
	{
		LastError.Empty();
	}
	const uint64 RequestedGeneration = ++Generation;
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::GitStatus, 0, 1);
	const FString ProjectFile = FPaths::GetProjectFilePath();
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	const TSharedRef<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> RefreshWorkerState = WorkerState.ToSharedRef();
	const TSharedRef<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = GameThreadDispatcher.ToSharedRef();
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	if (!RefreshWorkerState->TryBegin(CancellationContext, true))
	{
		FailRefresh(RequestedGeneration, TEXT("Changed Assets is shutting down and cannot start a refresh."));
		return;
	}

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
	EmitPendingRevertTelemetry(false);
	ClearPendingRefreshWork();
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
	RefreshPhase = EGitChangedAssetsRefreshPhase::Idle;
	RefreshProgressCompleted = 0;
	RefreshProgressTotal = 0;
	RowsChangedDelegate.Clear();
	ActivityChangedDelegate.Clear();
	Snapshot.Reset();
}

void FGitChangedAssetsController::HandleStartupGitCapabilityChanged()
{
	check(IsInGameThread());
	if (bShuttingDown.Load())
	{
		return;
	}
	LastError.Empty();
	ActivityChangedDelegate.Broadcast();
}

bool FGitChangedAssetsController::IsRefreshing() const
{
	return bRefreshing;
}

bool FGitChangedAssetsController::IsReverting() const
{
	return bReverting;
}

EGitChangedAssetsRefreshPhase FGitChangedAssetsController::GetRefreshPhase() const
{
	return RefreshPhase;
}

int32 FGitChangedAssetsController::GetRefreshProgressCompleted() const
{
	return RefreshProgressCompleted;
}

int32 FGitChangedAssetsController::GetRefreshProgressTotal() const
{
	return RefreshProgressTotal;
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
	if (bShuttingDown.Load() || bRefreshing || bReverting)
	{
		return;
	}
	if (!GitSourceControlUtils::IsStartupGitCapabilityAvailable())
	{
		LastError = GitSourceControlUtils::GetStartupGitCapabilityMessage().ToString();
		ActivityChangedDelegate.Broadcast();
		return;
	}
	if (!Snapshot.IsSet() || Entries.IsEmpty())
	{
		return;
	}

	FString EligibilityError;
	if (!GitChangedAssetOperations::FGitChangedAssetOperations::ValidateEntries(Entries, EligibilityError))
	{
		LastError = MoveTemp(EligibilityError);
		ActivityChangedDelegate.Broadcast();
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
	const TSharedRef<GitChangedAssetOperations::FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe> RevertTelemetry =
		MakeShared<GitChangedAssetOperations::FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe>();
	if (!RevertWorkerState->TryBegin(CancellationContext, true))
	{
		bReverting = false;
		LastError = TEXT("Changed Assets is shutting down and cannot start a revert.");
		ActivityChangedDelegate.Broadcast();
		return;
	}
	ActivityChangedDelegate.Broadcast();

	Async(EAsyncExecution::ThreadPool, [WeakController, GitBinary, RepositoryRoot, PinnedHead, Entries = MoveTemp(Entries), RevertWorkerState, Dispatcher, CancellationContext, RevertTelemetry]() mutable
	{
		GitChangedAssetsControllerPrivate::FScopedWorker Worker(RevertWorkerState, CancellationContext);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		const TSharedRef<GitChangedAssetOperations::FGitChangedAssetRevertLifecycle, ESPMode::ThreadSafe> Lifecycle =
			MakeShared<GitChangedAssetOperations::FGitChangedAssetRevertLifecycle, ESPMode::ThreadSafe>();
		GitChangedAssetOperations::FGitChangedAssetRevertCallbacks Callbacks;
		Callbacks.Telemetry = RevertTelemetry;
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
		Callbacks.FinalizeEditor = [Lifecycle, Dispatcher, RevertTelemetry](const TArray<FGitChangedAssetEntry>& CallbackEntries, const TArray<FString>& AffectedFiles,
			const GitChangedAssetOperations::EGitChangedAssetMutationOutcome Outcome, FString& OutError)
		{
			return GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [Lifecycle, RevertTelemetry, &CallbackEntries, &AffectedFiles, Outcome, &OutError]()
			{
				const bool bFinished = Lifecycle->Finish(CallbackEntries, AffectedFiles, Outcome, OutError);
				if (Outcome == GitChangedAssetOperations::EGitChangedAssetMutationOutcome::Succeeded)
				{
					const double AssetRegistryStartSeconds = FPlatformTime::Seconds();
					IAssetRegistry::GetChecked().ScanModifiedAssetFiles(AffectedFiles);
					RevertTelemetry->AssetRegistrySeconds += FPlatformTime::Seconds() - AssetRegistryStartSeconds;
					const double BrowserRefreshStartSeconds = FPlatformTime::Seconds();
					FEditorDelegates::RefreshAllBrowsers.Broadcast();
					RevertTelemetry->BrowserRefreshSeconds += FPlatformTime::Seconds() - BrowserRefreshStartSeconds;
				}
				return bFinished;
			});
		};

		GitChangedAssetOperations::FGitChangedAssetRevertResult Result;
		const bool bSucceeded = GitChangedAssetOperations::FGitChangedAssetOperations(GitBinary, RepositoryRoot)
			.RevertToHead(PinnedHead, Entries, Callbacks, Result);
		const bool bEditorReloadSucceeded = Result.bReloadSucceeded;
		const bool bCancelled = Result.bCancelled;
		FString ResultMessage = GitChangedAssetsControllerPrivate::MakeRevertResultMessage(Result);
		GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, bSucceeded, bEditorReloadSucceeded, bCancelled,
			ResultMessage = MoveTemp(ResultMessage), RevertTelemetry]() mutable
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteRevert(bSucceeded, bEditorReloadSucceeded, bCancelled, MoveTemp(ResultMessage), RevertTelemetry);
			}
			return true;
		});
	});
}

FGitChangedAssetsController::FOnChangedAssetsRowsUpdated& FGitChangedAssetsController::OnRowsChanged()
{
	return RowsChangedDelegate;
}

FGitChangedAssetsController::FOnChangedAssetsActivityUpdated& FGitChangedAssetsController::OnActivityChanged()
{
	return ActivityChangedDelegate;
}

void FGitChangedAssetsController::CompleteRefresh(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation)
	{
		return;
	}
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::GitStatus, 1, 1);
	const bool bPreserveLastError = bPreserveLastErrorForRefresh;
	bPreserveLastErrorForRefresh = false;
	if (!CompletedSnapshot.IsValid())
	{
		if (PendingRevertTelemetry.IsValid() && PendingRevertTelemetry->PostRefreshStartSeconds > 0.0)
		{
			PendingRevertTelemetry->PostRefreshStatusSeconds = FPlatformTime::Seconds() - PendingRevertTelemetry->PostRefreshStartSeconds;
		}
		FailRefresh(CompletedGeneration, Error.IsEmpty() ? TEXT("Git Changes refresh failed without a diagnostic.") : MoveTemp(Error));
		return;
	}
	if (PendingRevertTelemetry.IsValid() && PendingRevertTelemetry->PostRefreshStartSeconds > 0.0)
	{
		PendingRevertTelemetry->PostRefreshStatusSeconds = FPlatformTime::Seconds() - PendingRevertTelemetry->PostRefreshStartSeconds;
	}

	if (!bPreserveLastError)
	{
		LastError.Empty();
	}
	// Do not publish raw Git rows: untracked external packages have no usable package name until
	// the current-file resolver reads their exact .uasset package headers.
	BeginCurrentMetadata(CompletedGeneration, MoveTemp(CompletedSnapshot));
}

void FGitChangedAssetsController::BeginCurrentMetadata(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}
	FGitChangedAssetsMetadataResolver::BeginCurrentMetadata(*CompletedSnapshot);
	FPendingCurrentMetadataApply& Pending = PendingCurrentMetadataApply.Emplace();
	Pending.Generation = CompletedGeneration;
	Pending.Snapshot = MoveTemp(CompletedSnapshot);
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::CurrentMetadata, 0, Pending.Snapshot->Entries.Num());
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	if (!GameThreadDispatcher->Post([WeakController, CompletedGeneration]()
	{
		if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
		{
			Controller->ApplyPendingCurrentMetadata(CompletedGeneration);
		}
		return true;
	}))
	{
		FailRefresh(CompletedGeneration, TEXT("Changed Assets cannot schedule current metadata application."));
	}
}

void FGitChangedAssetsController::ApplyPendingCurrentMetadata(const uint64 PendingGeneration)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || PendingGeneration != Generation || !PendingCurrentMetadataApply.IsSet() ||
		PendingCurrentMetadataApply->Generation != PendingGeneration || !PendingCurrentMetadataApply->Snapshot.IsValid())
	{
		ClearPendingRefreshWork();
		if (!bShuttingDown.Load())
		{
			bRefreshing = false;
			SetRefreshActivity(EGitChangedAssetsRefreshPhase::Idle, 0, 0);
		}
		return;
	}

	FPendingCurrentMetadataApply& Pending = PendingCurrentMetadataApply.GetValue();
	const double BeginSeconds = FPlatformTime::Seconds();
	int32 ProcessedEntryCount = 0;
	while (Pending.NextEntryIndex < Pending.Snapshot->Entries.Num() &&
		ProcessedEntryCount < GitChangedAssetsControllerPrivate::CurrentMetadataApplyMaxEntriesPerTick)
	{
		if (ProcessedEntryCount > 0 && FPlatformTime::Seconds() - BeginSeconds >= GitChangedAssetsControllerPrivate::CurrentMetadataApplyTimeBudgetSeconds)
		{
			break;
		}
		const int32 AppliedCount = FGitChangedAssetsMetadataResolver::ApplyCurrentMetadataRange(*Pending.Snapshot, Pending.NextEntryIndex, 1);
		if (AppliedCount != 1)
		{
			Pending.NextEntryIndex = Pending.Snapshot->Entries.Num();
			break;
		}
		Pending.NextEntryIndex += AppliedCount;
		++ProcessedEntryCount;
	}
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::CurrentMetadata, Pending.NextEntryIndex, Pending.Snapshot->Entries.Num());

	if (Pending.NextEntryIndex < Pending.Snapshot->Entries.Num())
	{
		const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
		if (!GameThreadDispatcher->Post([WeakController, PendingGeneration]()
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->ApplyPendingCurrentMetadata(PendingGeneration);
			}
			return true;
		}))
		{
			FailRefresh(PendingGeneration, TEXT("Changed Assets cannot continue current metadata application."));
		}
		return;
	}

	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot = MoveTemp(Pending.Snapshot);
	PendingCurrentMetadataApply.Reset();
	CompleteCurrentMetadata(PendingGeneration, MoveTemp(CompletedSnapshot));
}

void FGitChangedAssetsController::CompleteCurrentMetadata(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}

	FGitChangedAssetsMetadataResolver::FinalizeCurrentMetadata(*CompletedSnapshot);

	const bool bNeedsHeadMetadata = CompletedSnapshot->Entries.ContainsByPredicate([](const FGitChangedAssetEntry& Entry)
	{
		return Entry.State == EGitChangedAssetState::Deleted ||
			(Entry.State == EGitChangedAssetState::Renamed && !Entry.bMetadataResolved && !FPaths::FileExists(Entry.AbsoluteFilename));
	});
	if (!bNeedsHeadMetadata)
	{
		BeginOwnerFallback(CompletedGeneration, MoveTemp(CompletedSnapshot), FString());
		return;
	}
	BeginHeadMetadata(CompletedGeneration, MoveTemp(CompletedSnapshot));
}

void FGitChangedAssetsController::BeginHeadMetadata(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::HeadMetadata, 0, 0);
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	const TSharedRef<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> HeadMetadataWorkerState = WorkerState.ToSharedRef();
	const TSharedRef<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = GameThreadDispatcher.ToSharedRef();
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	if (!HeadMetadataWorkerState->TryBegin(CancellationContext, true))
	{
		FailRefresh(CompletedGeneration, TEXT("Changed Assets is shutting down and cannot start HEAD metadata resolution."));
		return;
	}
	Async(EAsyncExecution::ThreadPool, [WeakController, CompletedGeneration, CompletedSnapshot, HeadMetadataWorkerState, Dispatcher, CancellationContext]()
	{
		GitChangedAssetsControllerPrivate::FScopedWorker Worker(HeadMetadataWorkerState, CancellationContext);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		FString HeadMetadataError;
		TArray<FGitChangedAssetHeadMetadataResult> HeadMetadataResults;
		FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(*CompletedSnapshot, HeadMetadataResults, HeadMetadataError, CancellationContext);
		GitChangedAssetsControllerPrivate::InvokeOnGameThreadAndWait(Dispatcher, [WeakController, CompletedGeneration, CompletedSnapshot,
			HeadMetadataResults = MoveTemp(HeadMetadataResults), HeadMetadataError = MoveTemp(HeadMetadataError)]() mutable
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->CompleteHeadMetadata(CompletedGeneration, CompletedSnapshot, MoveTemp(HeadMetadataResults), MoveTemp(HeadMetadataError));
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
	ClearPendingHeadMetadata();
	FPendingHeadMetadataApply& Pending = PendingHeadMetadataApply.Emplace();
	Pending.Generation = CompletedGeneration;
	Pending.Snapshot = MoveTemp(CompletedSnapshot);
	Pending.Results = MoveTemp(Results);
	Pending.Error = MoveTemp(Error);
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::HeadMetadata, 0, Pending.Results.Num());

	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	if (!GameThreadDispatcher->Post([WeakController, CompletedGeneration]()
	{
		if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
		{
			Controller->ApplyPendingHeadMetadata(CompletedGeneration);
		}
		return true;
	}))
	{
		FailRefresh(CompletedGeneration, TEXT("Changed Assets cannot schedule HEAD metadata application."));
	}
	return;
}

void FGitChangedAssetsController::ApplyPendingHeadMetadata(const uint64 PendingGeneration)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || PendingGeneration != Generation || !PendingHeadMetadataApply.IsSet() ||
		PendingHeadMetadataApply->Generation != PendingGeneration || !PendingHeadMetadataApply->Snapshot.IsValid())
	{
		ClearPendingRefreshWork();
		if (!bShuttingDown.Load())
		{
			bRefreshing = false;
			SetRefreshActivity(EGitChangedAssetsRefreshPhase::Idle, 0, 0);
		}
		return;
	}

	FPendingHeadMetadataApply& Pending = PendingHeadMetadataApply.GetValue();
	const double BeginSeconds = FPlatformTime::Seconds();
	int32 ProcessedResultCount = 0;
	while (Pending.NextResultIndex < Pending.Results.Num() && ProcessedResultCount < GitChangedAssetsControllerPrivate::HeadMetadataApplyMaxResultsPerTick)
	{
		if (ProcessedResultCount > 0 && FPlatformTime::Seconds() - BeginSeconds >= GitChangedAssetsControllerPrivate::HeadMetadataApplyTimeBudgetSeconds)
		{
			break;
		}
		const int32 AppliedCount = FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadataRange(*Pending.Snapshot, Pending.Results,
			Pending.NextResultIndex, 1);
		if (AppliedCount != 1)
		{
			Pending.NextResultIndex = Pending.Results.Num();
			break;
		}
		Pending.NextResultIndex += AppliedCount;
		++ProcessedResultCount;
	}
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::HeadMetadata, Pending.NextResultIndex, Pending.Results.Num());

	if (Pending.NextResultIndex < Pending.Results.Num())
	{
		const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
		if (!GameThreadDispatcher->Post([WeakController, PendingGeneration]()
		{
			if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
			{
				Controller->ApplyPendingHeadMetadata(PendingGeneration);
			}
			return true;
		}))
		{
			FailRefresh(PendingGeneration, TEXT("Changed Assets cannot continue HEAD metadata application."));
		}
		return;
	}

	if (bShuttingDown.Load() || PendingGeneration != Generation || PendingHeadMetadataApply->Generation != PendingGeneration)
	{
		ClearPendingRefreshWork();
		if (!bShuttingDown.Load())
		{
			bRefreshing = false;
			SetRefreshActivity(EGitChangedAssetsRefreshPhase::Idle, 0, 0);
		}
		return;
	}

	FGitChangedAssetsMetadataResolver::FinalizeHeadOnlyMetadata(*Pending.Snapshot);
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot = MoveTemp(Pending.Snapshot);
	FString Error = MoveTemp(Pending.Error);
	ClearPendingHeadMetadata();
	BeginOwnerFallback(PendingGeneration, MoveTemp(CompletedSnapshot), MoveTemp(Error));
}

void FGitChangedAssetsController::BeginOwnerFallback(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString ExistingError)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}
	FPendingOwnerFallback& Pending = PendingOwnerFallback.Emplace();
	Pending.Generation = CompletedGeneration;
	Pending.Snapshot = MoveTemp(CompletedSnapshot);
	Pending.Error = MoveTemp(ExistingError);
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::OwnerFallback, 0, 1);
	const TWeakPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> WeakController = AsShared();
	if (!GameThreadDispatcher->Post([WeakController, CompletedGeneration]()
	{
		if (const TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> Controller = WeakController.Pin())
		{
			Controller->CompleteOwnerFallback(CompletedGeneration);
		}
		return true;
	}))
	{
		FailRefresh(CompletedGeneration, TEXT("Changed Assets cannot schedule owner fallback."));
	}
}

void FGitChangedAssetsController::ClearPendingHeadMetadata()
{
	PendingHeadMetadataApply.Reset();
}

void FGitChangedAssetsController::CompleteOwnerFallback(const uint64 CompletedGeneration)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !PendingOwnerFallback.IsSet() ||
		PendingOwnerFallback->Generation != CompletedGeneration || !PendingOwnerFallback->Snapshot.IsValid())
	{
		return;
	}
	FGitChangedAssetsMetadataResolver::ResolveOutstandingOwnerFallback(*PendingOwnerFallback->Snapshot);
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot = MoveTemp(PendingOwnerFallback->Snapshot);
	FString Error = MoveTemp(PendingOwnerFallback->Error);
	PendingOwnerFallback.Reset();
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::OwnerFallback, 1, 1);
	FinishRefresh(CompletedGeneration, MoveTemp(CompletedSnapshot), MoveTemp(Error));
}

void FGitChangedAssetsController::FinishRefresh(const uint64 CompletedGeneration,
	TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation || !CompletedSnapshot.IsValid())
	{
		return;
	}
	Snapshot = MoveTemp(*CompletedSnapshot);
	LastSuccessfulRefreshTime = FDateTime::UtcNow();
	bRefreshing = false;
	if (!Error.IsEmpty())
	{
		LastError = LastError.IsEmpty() ? MoveTemp(Error) : LastError + TEXT("\n") + Error;
	}
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::Idle, 0, 0);
	RowsChangedDelegate.Broadcast();
	EmitPendingRevertTelemetry(true);
}

void FGitChangedAssetsController::FailRefresh(const uint64 CompletedGeneration, FString Error)
{
	check(IsInGameThread());
	if (bShuttingDown.Load() || CompletedGeneration != Generation)
	{
		return;
	}
	ClearPendingRefreshWork();
	bRefreshing = false;
	bPreserveLastErrorForRefresh = false;
	LastError = Error.IsEmpty() ? TEXT("Git Changes refresh failed without a diagnostic.") : MoveTemp(Error);
	SetRefreshActivity(EGitChangedAssetsRefreshPhase::Idle, 0, 0);
	EmitPendingRevertTelemetry(false);
}

void FGitChangedAssetsController::EmitPendingRevertTelemetry(const bool bPostRefreshSucceeded)
{
	if (!PendingRevertTelemetry.IsValid())
	{
		return;
	}

	const TSharedPtr<GitChangedAssetOperations::FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe> Telemetry = MoveTemp(PendingRevertTelemetry);
	if (Telemetry->PostRefreshStartSeconds > 0.0)
	{
		const double NowSeconds = FPlatformTime::Seconds();
		Telemetry->PostRefreshTotalSeconds = NowSeconds - Telemetry->PostRefreshStartSeconds;
		Telemetry->PostRefreshMetadataSeconds = Telemetry->PostRefreshStatusSeconds > 0.0
			? FMath::Max(0.0, Telemetry->PostRefreshTotalSeconds - Telemetry->PostRefreshStatusSeconds)
			: 0.0;
	}
	Telemetry->bPostRefreshSucceeded = bPostRefreshSucceeded;
	GitChangedAssetsControllerPrivate::EmitRevertTelemetry(Telemetry);
}

void FGitChangedAssetsController::ClearPendingRefreshWork()
{
	PendingCurrentMetadataApply.Reset();
	ClearPendingHeadMetadata();
	PendingOwnerFallback.Reset();
}

void FGitChangedAssetsController::SetRefreshActivity(const EGitChangedAssetsRefreshPhase InPhase, const int32 InCompleted, const int32 InTotal)
{
	const int32 Completed = FMath::Max(0, InCompleted);
	const int32 Total = FMath::Max(0, InTotal);
	if (RefreshPhase == InPhase && RefreshProgressCompleted == Completed && RefreshProgressTotal == Total)
	{
		return;
	}
	RefreshPhase = InPhase;
	RefreshProgressCompleted = Completed;
	RefreshProgressTotal = Total;
	ActivityChangedDelegate.Broadcast();
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

void FGitChangedAssetsController::CompleteRevert(const bool bDiskMutationSucceeded, const bool bEditorReloadSucceeded, const bool bCancelled, FString ResultMessage,
	TSharedPtr<GitChangedAssetOperations::FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe> RevertTelemetry)
{
	check(IsInGameThread());
	if (RevertTelemetry.IsValid())
	{
		RevertTelemetry->bDiskMutationSucceeded = bDiskMutationSucceeded;
		RevertTelemetry->bEditorReloadSucceeded = bEditorReloadSucceeded;
		RevertTelemetry->bCancelled = bCancelled;
	}
	if (bShuttingDown.Load())
	{
		GitChangedAssetsControllerPrivate::EmitRevertTelemetry(RevertTelemetry);
		return;
	}
	bReverting = false;
	if (!bDiskMutationSucceeded)
	{
		LastError = MoveTemp(ResultMessage);
		ActivityChangedDelegate.Broadcast();
		GitChangedAssetsControllerPrivate::EmitRevertTelemetry(RevertTelemetry);
		return;
	}
	PendingRevertTelemetry = MoveTemp(RevertTelemetry);
	if (PendingRevertTelemetry.IsValid())
	{
		PendingRevertTelemetry->bPostRefreshAttempted = true;
		PendingRevertTelemetry->PostRefreshStartSeconds = FPlatformTime::Seconds();
	}

	if (!bEditorReloadSucceeded)
	{
		LastError = MoveTemp(ResultMessage);
		ActivityChangedDelegate.Broadcast();
		Refresh(false);
		return;
	}

	LastError.Empty();
	ActivityChangedDelegate.Broadcast();
	Refresh();
}

#undef LOCTEXT_NAMESPACE
