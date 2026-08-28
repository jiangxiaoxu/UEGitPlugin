// Copyright (c) 2026

#include "GitLocalSourceControl.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "GitSourceControlAssetOperations.h"
#include "GitSourceControlRevision.h"
#include "GitStandaloneLog.h"
#include "GitStandaloneHistory.h"
#include "GitSourceControlUtils.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/GCObject.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace GitLocalSourceControlPrivate
{
	class FBlockedReadOnlyOperationGate;
}
#endif

namespace GitLocalSourceControlPrivate
{
	enum class EOperationTelemetryKind : uint8
	{
		Unknown,
		LoadHistory,
		FetchLfsRevision,
		RestoreRevision,
		DiscardTracked,
		DeferredDiscardTracked,
		TestOnly,
	};

	enum class EOperationTerminalReason : uint8
	{
		None,
		Completed,
		Cancelled,
		WorkerFailed,
		ReloadFailed,
	};

	constexpr int32 OperationPhaseCount = static_cast<int32>(EGitLocalSourceControlOperationPhase::Failed) + 1;
}

struct FGitLocalSourceControlOperationState final
{
	explicit FGitLocalSourceControlOperationState(const GitLocalSourceControlPrivate::EOperationTelemetryKind InOperationKind = GitLocalSourceControlPrivate::EOperationTelemetryKind::Unknown,
		const int32 InRequestedTargetCount = 0)
		: OperationKind(InOperationKind)
		, RequestedTargetCount(InRequestedTargetCount)
		, RegisteredSeconds(FPlatformTime::Seconds())
		, LastPhaseStartSeconds(RegisteredSeconds)
	{
		PhaseSeconds.Init(0.0, GitLocalSourceControlPrivate::OperationPhaseCount);
	}

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
	TArray<FString> ReloadFilenames;
	TArray<EGitLocalSourceControlOperationPhase> PendingProgressPhases;
	TArray<FString> DeferredCleanupAssetObjectPaths;
	bool bCancellationAllowed = true;
	bool bCanScheduleDeferredDiscard = false;
	bool bDiskMutationCommitted = false;
	bool bInternalDeferredCleanup = false;
	bool bWorkerFinished = false;
	GitLocalSourceControlPrivate::EOperationTelemetryKind OperationKind = GitLocalSourceControlPrivate::EOperationTelemetryKind::Unknown;
	GitLocalSourceControlPrivate::EOperationTerminalReason TerminalReason = GitLocalSourceControlPrivate::EOperationTerminalReason::None;
	int32 RequestedTargetCount = 0;
	int32 AffectedFileCount = 0;
	int32 HistoryEntryCount = 0;
	double RegisteredSeconds = 0.0;
	double WorkerStartedSeconds = 0.0;
	double WorkerFinishedSeconds = 0.0;
	double GameThreadFinalizeStartedSeconds = 0.0;
	double ReloadStartedSeconds = 0.0;
	double TerminalPublishedSeconds = 0.0;
	double LastPhaseStartSeconds = 0.0;
	TArray<double, TInlineAllocator<GitLocalSourceControlPrivate::OperationPhaseCount>> PhaseSeconds;
	bool bDeferredCleanupRequested = false;
	bool bDeferredCleanupLaunchAttempted = false;
	bool bTerminalTelemetryEmitted = false;

#if WITH_DEV_AUTOMATION_TESTS
	TSharedPtr<GitLocalSourceControlPrivate::FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> FinalPublicationGate;
#endif
};

namespace GitLocalSourceControlPrivate
{
	void AccumulateCurrentPhaseLocked(FGitLocalSourceControlOperationState& InOutState, const double NowSeconds)
	{
		const int32 PhaseIndex = static_cast<int32>(InOutState.Phase);
		if (InOutState.PhaseSeconds.IsValidIndex(PhaseIndex) && NowSeconds >= InOutState.LastPhaseStartSeconds)
		{
			InOutState.PhaseSeconds[PhaseIndex] += NowSeconds - InOutState.LastPhaseStartSeconds;
		}
		InOutState.LastPhaseStartSeconds = NowSeconds;
	}

	void MarkWorkerStarted(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		FScopeLock Lock(&InState->Mutex);
		InState->WorkerStartedSeconds = FPlatformTime::Seconds();
	}

	const TCHAR* GetOperationTelemetryKindName(const EOperationTelemetryKind InKind)
	{
		switch (InKind)
		{
		case EOperationTelemetryKind::LoadHistory: return TEXT("load_history");
		case EOperationTelemetryKind::FetchLfsRevision: return TEXT("fetch_lfs_revision");
		case EOperationTelemetryKind::RestoreRevision: return TEXT("restore_revision");
		case EOperationTelemetryKind::DiscardTracked: return TEXT("discard_tracked");
		case EOperationTelemetryKind::DeferredDiscardTracked: return TEXT("deferred_discard_tracked");
		case EOperationTelemetryKind::TestOnly: return TEXT("test_only");
		default: return TEXT("unknown");
		}
	}

	const TCHAR* GetOperationTerminalReasonName(const EOperationTerminalReason InReason)
	{
		switch (InReason)
		{
		case EOperationTerminalReason::Completed: return TEXT("completed");
		case EOperationTerminalReason::Cancelled: return TEXT("cancelled");
		case EOperationTerminalReason::WorkerFailed: return TEXT("worker_failed");
		case EOperationTerminalReason::ReloadFailed: return TEXT("reload_failed");
		default: return TEXT("unknown");
		}
	}

	void EmitTerminalTelemetry(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		EOperationTelemetryKind OperationKind = EOperationTelemetryKind::Unknown;
		EOperationTerminalReason TerminalReason = EOperationTerminalReason::None;
		int32 RequestedTargetCount = 0;
		int32 AffectedFileCount = 0;
		int32 HistoryEntryCount = 0;
		bool bInternalDeferredCleanup = false;
		bool bDeferredCleanupRequested = false;
		bool bDeferredCleanupLaunchAttempted = false;
		double RegisteredSeconds = 0.0;
		double WorkerStartedSeconds = 0.0;
		double WorkerFinishedSeconds = 0.0;
		double GameThreadFinalizeStartedSeconds = 0.0;
		double ReloadStartedSeconds = 0.0;
		double TerminalPublishedSeconds = 0.0;
		TArray<double, TInlineAllocator<OperationPhaseCount>> PhaseSeconds;
		{
			FScopeLock Lock(&InState->Mutex);
			if (InState->bTerminalTelemetryEmitted)
			{
				return;
			}
			InState->bTerminalTelemetryEmitted = true;
			OperationKind = InState->OperationKind;
			TerminalReason = InState->TerminalReason;
			RequestedTargetCount = InState->RequestedTargetCount;
			AffectedFileCount = InState->AffectedFileCount;
			HistoryEntryCount = InState->HistoryEntryCount;
			bInternalDeferredCleanup = InState->bInternalDeferredCleanup;
			bDeferredCleanupRequested = InState->bDeferredCleanupRequested;
			bDeferredCleanupLaunchAttempted = InState->bDeferredCleanupLaunchAttempted;
			RegisteredSeconds = InState->RegisteredSeconds;
			WorkerStartedSeconds = InState->WorkerStartedSeconds;
			WorkerFinishedSeconds = InState->WorkerFinishedSeconds;
			GameThreadFinalizeStartedSeconds = InState->GameThreadFinalizeStartedSeconds;
			ReloadStartedSeconds = InState->ReloadStartedSeconds;
			TerminalPublishedSeconds = InState->TerminalPublishedSeconds;
			PhaseSeconds = InState->PhaseSeconds;
		}

		const auto GetPhaseSeconds = [&PhaseSeconds](const EGitLocalSourceControlOperationPhase InPhase)
		{
			const int32 PhaseIndex = static_cast<int32>(InPhase);
			return PhaseSeconds.IsValidIndex(PhaseIndex) ? PhaseSeconds[PhaseIndex] : 0.0;
		};
		const double QueueToWorkerSeconds = WorkerStartedSeconds > 0.0 ? FMath::Max(0.0, WorkerStartedSeconds - RegisteredSeconds) : 0.0;
		const double WorkerSeconds = WorkerStartedSeconds > 0.0 && WorkerFinishedSeconds > 0.0 ? FMath::Max(0.0, WorkerFinishedSeconds - WorkerStartedSeconds) : 0.0;
		const double WorkerToGameThreadSeconds = WorkerFinishedSeconds > 0.0 && GameThreadFinalizeStartedSeconds > 0.0
			? FMath::Max(0.0, GameThreadFinalizeStartedSeconds - WorkerFinishedSeconds) : 0.0;
		const double GameThreadFinalizeSeconds = GameThreadFinalizeStartedSeconds > 0.0 && TerminalPublishedSeconds > 0.0
			? FMath::Max(0.0, TerminalPublishedSeconds - GameThreadFinalizeStartedSeconds) : 0.0;
		const double ReloadSeconds = ReloadStartedSeconds > 0.0 && TerminalPublishedSeconds > 0.0
			? FMath::Max(0.0, TerminalPublishedSeconds - ReloadStartedSeconds) : 0.0;
		const double TotalSeconds = TerminalPublishedSeconds > 0.0 ? FMath::Max(0.0, TerminalPublishedSeconds - RegisteredSeconds) : 0.0;
		UE_LOG(LogGitStandalone, Verbose, TEXT("Git Local SourceControl operation timing: kind=%s terminalReason=%s targets=%d affected=%d history=%d internalDeferredCleanup=%d deferredRequested=%d deferredChildLaunchAttempted=%d queueToWorker=%.3fs worker=%.3fs workerToGameThread=%.3fs gameThreadFinalize=%.3fs reload=%.3fs queued=%.3fs loadingHistory=%.3fs fetchingLfs=%.3fs preparing=%.3fs mutating=%.3fs reloading=%.3fs total=%.3fs"),
			GetOperationTelemetryKindName(OperationKind), GetOperationTerminalReasonName(TerminalReason), RequestedTargetCount, AffectedFileCount, HistoryEntryCount,
			bInternalDeferredCleanup, bDeferredCleanupRequested, bDeferredCleanupLaunchAttempted, QueueToWorkerSeconds, WorkerSeconds,
			WorkerToGameThreadSeconds, GameThreadFinalizeSeconds, ReloadSeconds, GetPhaseSeconds(EGitLocalSourceControlOperationPhase::Queued),
			GetPhaseSeconds(EGitLocalSourceControlOperationPhase::LoadingHistory), GetPhaseSeconds(EGitLocalSourceControlOperationPhase::FetchingLfs),
			GetPhaseSeconds(EGitLocalSourceControlOperationPhase::Preparing), GetPhaseSeconds(EGitLocalSourceControlOperationPhase::Mutating),
			GetPhaseSeconds(EGitLocalSourceControlOperationPhase::Reloading), TotalSeconds);
	}

	bool ReloadPreparedPackages(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, TArray<FString>* OutErrors = nullptr);
	bool RegisterManagedOperation(UGitLocalSourceControlOperation* InOperation, bool bAllowDuringShutdown = false);
	UGitLocalSourceControlOperation* StartDiscardTrackedInternal(const TArray<FString>& AssetObjectPaths, bool bAllowDuringShutdown, bool bInternalDeferredCleanup);

#if WITH_DEV_AUTOMATION_TESTS
	TAtomic<bool> GForcePreparedPackageReloadFailureForTesting = false;
	TAtomic<bool> GForceRestoreWorktreeRollbackForTesting = false;

	void SetForcePreparedPackageReloadFailureForTesting(const bool bEnabled)
	{
		GForcePreparedPackageReloadFailureForTesting.Store(bEnabled);
	}

	void SetForceRestoreWorktreeRollbackForTesting(const bool bEnabled)
	{
		GForceRestoreWorktreeRollbackForTesting.Store(bEnabled);
	}

	FString GLastDeferredCleanupDiagnostic;
	TAtomic<int32> GDeferredCleanupLaunchCount = 0;

	class FBlockedReadOnlyOperationGate final
	{
	public:
		FEventRef WorkerReachedFinalPublication{ EEventMode::ManualReset };
		FEventRef ReleaseFinalPublication{ EEventMode::ManualReset };
	};

	TSharedPtr<FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> GBlockedReadOnlyOperationGate;

	void SetLastDeferredCleanupDiagnostic(FString InDiagnostic)
	{
		GLastDeferredCleanupDiagnostic = MoveTemp(InDiagnostic);
	}
#endif

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

		bool TryBegin(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, const bool bAllowDuringShutdown = false)
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingNewWorkers && !bAllowDuringShutdown)
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

		void BeginShutdown()
		{
			check(IsInGameThread());
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

			WaitForAllWorkers();
		}

		void WaitForAllWorkers()
		{
			check(IsInGameThread());
			while (!AllWorkersFinished->Wait(10))
			{}
		}

	private:
		FCriticalSection Mutex;
		FEvent* AllWorkersFinished = nullptr;
		bool bAcceptingNewWorkers = true;
		TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> ActiveStates;
	};

	FOperationRegistry& GetOperationRegistry()
	{
		static FOperationRegistry Registry;
		return Registry;
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

	bool ResolveTarget(const FString& InAssetObjectPath, const ETargetAccess InAccess, FResolvedTarget& OutTarget, FString& OutError)
	{
		check(IsInGameThread());
		static_cast<void>(InAccess);
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
			// Explicit history and mutation validation must still resolve a tracked
			// package path when the worktree file is currently absent.
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
		OutTarget.ObjectPath = ObjectPath;
		OutTarget.PackageName = PackageName;
		OutTarget.Filename = MoveTemp(Filename);
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

		return true;
	}

	bool ResolveRepositoryForTarget(FResolvedTarget& InOutTarget, FString& OutError)
	{
		OutError.Reset();
		return GitSourceControlUtils::ResolveStandaloneRepositoryForFile(InOutTarget.Filename,
			InOutTarget.GitBinary, InOutTarget.RepositoryRoot, OutError);
	}

	FGitLocalSourceControlProviderInfo GetProviderInfoOnGameThread(const FString& InAssetObjectPath)
	{
		check(IsInGameThread());
		FGitLocalSourceControlProviderInfo Info;
		FResolvedTarget Target;
		if (!ResolveTarget(InAssetObjectPath, ETargetAccess::ReadOnly, Target, Info.Error))
		{
			return Info;
		}
		Info.bAvailable = ResolveRepositoryForTarget(Target, Info.Error);
		if (Info.bAvailable)
		{
			Info.GitBinary = MoveTemp(Target.GitBinary);
			Info.RepositoryRoot = MoveTemp(Target.RepositoryRoot);
		}
		else if (Info.Error.IsEmpty())
		{
			Info.Error = TEXT("Could not resolve a local Git executable and repository root for the asset.");
		}
		return Info;
	}

	bool ResolveRepositoriesForTargets(TArray<FResolvedTarget>& InOutTargets, FString& OutError)
	{
		OutError.Reset();
		if (InOutTargets.IsEmpty())
		{
			OutError = TEXT("At least one asset target is required.");
			return false;
		}
		for (FResolvedTarget& Target : InOutTargets)
		{
			if (!ResolveRepositoryForTarget(Target, OutError))
			{
				return false;
			}
		}
		const FString& RepositoryRoot = InOutTargets[0].RepositoryRoot;
		for (const FResolvedTarget& Target : InOutTargets)
		{
			if (!Target.RepositoryRoot.Equals(RepositoryRoot, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("One scripted operation cannot mutate assets from multiple Git repositories.");
				return false;
			}
		}
		return true;
	}

	bool ReloadPreparedPackages(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, TArray<FString>* OutErrors)
	{
		check(IsInGameThread());
		bool bReloadSucceeded = true;
#if WITH_DEV_AUTOMATION_TESTS
		if (GForcePreparedPackageReloadFailureForTesting.Load())
		{
			bReloadSucceeded = false;
			if (OutErrors != nullptr)
			{
				OutErrors->Add(TEXT("Automation test seam forced prepared package reload failure."));
			}
		}
#endif

		TArray<FString> Filenames;
		{
			FScopeLock Lock(&InState->Mutex);
			Filenames = MoveTemp(InState->ReloadFilenames);
		}
		for (const FString& Filename : Filenames)
		{
			FString PackageName;
			if (!FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				bReloadSucceeded = false;
				if (OutErrors != nullptr)
				{
					OutErrors->Add(FString::Printf(TEXT("Could not resolve the reloaded package name for %s."), *Filename));
				}
				continue;
			}
			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr || Package->FindAssetInPackage() == nullptr)
			{
				bReloadSucceeded = false;
				if (OutErrors != nullptr)
				{
					OutErrors->Add(FString::Printf(TEXT("Could not reload the Editor package %s."), *PackageName));
				}
			}
		}
		return bReloadSucceeded;
	}

	bool PrepareLoadedPackagesForMutation(const TArray<FResolvedTarget>& InTargets,
		const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, const bool bAllowDirtyPackages, FString& OutError)
	{
		check(IsInGameThread());
		TArray<FString> Filenames;
		TArray<UPackage*> LoadedPackages;
		for (const FResolvedTarget& Target : InTargets)
		{
			Filenames.Add(Target.Filename);
			if (UPackage* Package = FindPackage(nullptr, *Target.PackageName))
			{
				if (Package->IsDirty() && !bAllowDirtyPackages)
				{
					OutError = FString::Printf(TEXT("The asset package has unsaved Editor changes and cannot be mutated: %s"), *Target.PackageName);
					return false;
				}
				LoadedPackages.Add(Package);
			}
		}
		if (!GitSourceControlAssetOperations::FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(Filenames, LoadedPackages, OutError))
		{
			return false;
		}
		if (LoadedPackages.IsEmpty())
		{
			return true;
		}
		{
			FScopeLock Lock(&InState->Mutex);
			InState->ReloadFilenames = Filenames;
		}
		FText UnloadError;
		if (!UPackageTools::UnloadPackages(LoadedPackages, UnloadError, bAllowDirtyPackages))
		{
			OutError = UnloadError.IsEmpty() ? TEXT("Could not unload loaded asset packages before mutation.") : UnloadError.ToString();
			return false;
		}
		return true;
	}

	void SetWorkerPhase(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState,
		const EGitLocalSourceControlOperationPhase InPhase, const bool bInCancellationAllowed)
	{
		FScopeLock Lock(&InState->Mutex);
		if (InState->Phase != InPhase)
		{
			AccumulateCurrentPhaseLocked(*InState, FPlatformTime::Seconds());
			InState->Phase = InPhase;
			InState->PendingProgressPhases.Add(InPhase);
		}
		InState->bCancellationAllowed = bInCancellationAllowed;
	}

	bool CanEnterMutation(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		FScopeLock Lock(&InState->Mutex);
		if (InState->CancellationContext->IsCancellationRequested())
		{
			return false;
		}
		if (InState->Phase != EGitLocalSourceControlOperationPhase::Mutating)
		{
			AccumulateCurrentPhaseLocked(*InState, FPlatformTime::Seconds());
			InState->Phase = EGitLocalSourceControlOperationPhase::Mutating;
			InState->PendingProgressPhases.Add(EGitLocalSourceControlOperationPhase::Mutating);
		}
		InState->bCancellationAllowed = false;
		return true;
	}

	void MarkDiskMutationCommitted(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		FScopeLock Lock(&InState->Mutex);
		InState->bDiskMutationCommitted = true;
	}

	void FinishWorker(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState, FGitLocalSourceControlOperationResult&& InResult)
	{
#if WITH_DEV_AUTOMATION_TESTS
		TSharedPtr<FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> FinalPublicationGate;
		{
			FScopeLock Lock(&InState->Mutex);
			FinalPublicationGate = InState->FinalPublicationGate;
		}
		if (FinalPublicationGate.IsValid())
		{
			FinalPublicationGate->WorkerReachedFinalPublication->Trigger();
			FinalPublicationGate->ReleaseFinalPublication->Wait();
		}
#endif

		{
			FScopeLock Lock(&InState->Mutex);
			const double NowSeconds = FPlatformTime::Seconds();
			const bool bCancelled = InResult.bCancelled || InState->CancellationContext->IsCancellationRequested();
			InResult.bCancelled = bCancelled;
			if (bCancelled)
			{
				InResult.bSucceeded = false;
			}
			InState->Result = MoveTemp(InResult);
			InState->bCancellationAllowed = false;
			if (InState->WorkerStartedSeconds <= 0.0)
			{
				InState->WorkerStartedSeconds = NowSeconds;
			}
			AccumulateCurrentPhaseLocked(*InState, NowSeconds);
			InState->WorkerFinishedSeconds = NowSeconds;
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

	UGitLocalSourceControlOperation* MakeOperation(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState, const bool bAllowDuringShutdown = false)
	{
		check(IsInGameThread());
		UGitLocalSourceControlOperation* Operation = NewObject<UGitLocalSourceControlOperation>(GetTransientPackage());
		Operation->Initialize(MoveTemp(InState));
		return RegisterManagedOperation(Operation, bAllowDuringShutdown) ? Operation : nullptr;
	}

	UGitLocalSourceControlOperation* MakeFailedOperation(const FString& InError, const EOperationTelemetryKind InOperationKind,
		const int32 InRequestedTargetCount, const bool bAllowDuringShutdown = false, const bool bInternalDeferredCleanup = false,
		const TArray<FString>& DeferredCleanupAssetObjectPaths = {})
	{
		TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
			InOperationKind, InRequestedTargetCount);
		State->bInternalDeferredCleanup = bInternalDeferredCleanup;
		State->DeferredCleanupAssetObjectPaths = DeferredCleanupAssetObjectPaths;
		FGitLocalSourceControlOperationResult Result;
		Result.Errors.Add(InError);
		FinishWorker(State, MoveTemp(Result));
		return MakeOperation(State, bAllowDuringShutdown);
	}

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakePreauthorizedClosedAssetCallbacks(
		const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		GitSourceControlAssetOperations::FGitAssetOperationCallbacks Callbacks;
		Callbacks.IsCancellationRequested = [InState]()
		{
			return InState->CancellationContext->IsCancellationRequested();
		};
		Callbacks.Confirm = [](const FString&, const TArray<FString>&)
		{
			return true;
		};
		Callbacks.PrepareForMutation = [InState](const TArray<FString>&)
		{
			return CanEnterMutation(InState);
		};
		Callbacks.ReloadPackages = [InState](const TArray<FString>&)
		{
			// Start-time validation rejects loaded packages. UI-level reload is scheduled
			// after this operation reaches a terminal result on the Game Thread.
			SetWorkerPhase(InState, EGitLocalSourceControlOperationPhase::Reloading, false);
			return true;
		};
#if WITH_DEV_AUTOMATION_TESTS
		Callbacks.AllowWorktreeReplaceForTesting = []()
		{
			return !GForceRestoreWorktreeRollbackForTesting.Load();
		};
#endif
		return Callbacks;
	}

	class FOperationManager final : public FGCObject
	{
	public:
		void Startup()
		{
			check(IsInGameThread());
			check(!bShuttingDown);
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateRaw(this, &FOperationManager::Tick), 0.0f);
			}
		}

		bool Register(UGitLocalSourceControlOperation* InOperation, const bool bAllowDuringShutdown)
		{
			check(IsInGameThread());
			check(InOperation != nullptr);
			if ((bShuttingDown && !bAllowDuringShutdown) || (!TickerHandle.IsValid() && !bAllowDuringShutdown))
			{
				return false;
			}
			ManagedOperations.Add(InOperation);
			return true;
		}

		void PumpOperations(const bool bBroadcastNotifications, const bool bAllowShutdownCleanup)
		{
			check(IsInGameThread());
			TArray<TObjectPtr<UGitLocalSourceControlOperation>> Snapshot;
			TArray<TObjectPtr<UGitLocalSourceControlOperation>> DeferredRemovals;
			Snapshot.Reserve(ManagedOperations.Num());
			DeferredRemovals.Reserve(ManagedOperations.Num());
			for (const TObjectPtr<UGitLocalSourceControlOperation>& Operation : ManagedOperations)
			{
				Snapshot.Add(Operation);
			}
			for (UGitLocalSourceControlOperation* Operation : Snapshot)
			{
				if (Operation != nullptr && ManagedOperations.Contains(Operation))
				{
					Operation->PumpOnGameThread(bBroadcastNotifications, bAllowShutdownCleanup);
				}
				if (Operation == nullptr || Operation->IsTerminal())
				{
					DeferredRemovals.Add(Operation);
				}
			}
			for (const TObjectPtr<UGitLocalSourceControlOperation>& Operation : DeferredRemovals)
			{
				ManagedOperations.Remove(Operation);
			}
		}

		void Shutdown()
		{
			check(IsInGameThread());
			bShuttingDown = true;
			StopTicker();
			while (!ManagedOperations.IsEmpty())
			{
				GetOperationRegistry().WaitForAllWorkers();
				PumpOperations(false, true);
			}
		}

#if WITH_DEV_AUTOMATION_TESTS
		void PumpForTesting()
		{
			PumpOperations(true, false);
		}

		bool DrainForTesting()
		{
			check(IsInGameThread());
			const bool bTickerWasRegistered = TickerHandle.IsValid();
			StopTicker();
			while (!ManagedOperations.IsEmpty())
			{
				GetOperationRegistry().WaitForAllWorkers();
				PumpOperations(false, true);
			}
			Startup();
			return bTickerWasRegistered && TickerHandle.IsValid();
		}
#endif

		int32 GetManagedOperationCount() const
		{
			check(IsInGameThread());
			return ManagedOperations.Num();
		}

		bool HasTicker() const
		{
			check(IsInGameThread());
			return TickerHandle.IsValid();
		}

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObjects(ManagedOperations);
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("GitLocalSourceControlOperationManager");
		}

	private:
		bool Tick(float DeltaTime)
		{
			static_cast<void>(DeltaTime);
			if (!ManagedOperations.IsEmpty())
			{
				PumpOperations(true, false);
			}
			return true;
		}

		void StopTicker()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}

		TSet<TObjectPtr<UGitLocalSourceControlOperation>> ManagedOperations;
		FTSTicker::FDelegateHandle TickerHandle;
		bool bShuttingDown = false;
	};

	FOperationManager& GetOperationManager()
	{
		static FOperationManager Manager;
		return Manager;
	}

	bool RegisterManagedOperation(UGitLocalSourceControlOperation* InOperation, const bool bAllowDuringShutdown)
	{
		return GetOperationManager().Register(InOperation, bAllowDuringShutdown);
	}
}

void UGitLocalSourceControlOperation::Initialize(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState)
{
	check(IsInGameThread());
	State = MoveTemp(InState);
}

void UGitLocalSourceControlOperation::PumpOnGameThread(const bool bBroadcastNotifications, const bool bAllowShutdownCleanup)
{
	check(IsInGameThread());
	if (!State.IsValid() || bTerminal)
	{
		return;
	}

	TArray<EGitLocalSourceControlOperationPhase> ProgressPhases;
	bool bWorkerFinished = false;
	bool bDiskMutationCommitted = false;
	bool bNeedsReload = false;
	{
		FScopeLock Lock(&State->Mutex);
		ProgressPhases = MoveTemp(State->PendingProgressPhases);
		bWorkerFinished = State->bWorkerFinished;
		if (bWorkerFinished)
		{
			Result = State->Result;
			bDiskMutationCommitted = State->bDiskMutationCommitted;
		}
		bNeedsReload = !State->ReloadFilenames.IsEmpty();
	}
	for (const EGitLocalSourceControlOperationPhase ProgressPhase : ProgressPhases)
	{
		Phase = ProgressPhase;
		if (bBroadcastNotifications)
		{
			OnProgress.Broadcast(ProgressPhase);
		}
	}
	if (!bWorkerFinished)
	{
		return;
	}
	{
		FScopeLock Lock(&State->Mutex);
		if (State->GameThreadFinalizeStartedSeconds <= 0.0)
		{
			State->GameThreadFinalizeStartedSeconds = FPlatformTime::Seconds();
		}
	}

	if (bNeedsReload)
	{
		{
			FScopeLock Lock(&State->Mutex);
			State->ReloadStartedSeconds = FPlatformTime::Seconds();
		}
		TArray<FString> ReloadErrors;
		Result.bReloadSucceeded = GitLocalSourceControlPrivate::ReloadPreparedPackages(State.ToSharedRef(), &ReloadErrors);
		if (!Result.bReloadSucceeded)
		{
			Result.Errors.Append(MoveTemp(ReloadErrors));
			if (Result.bSucceeded)
			{
				Result.Errors.Add(TEXT("Git changed the asset file on disk, but the Editor package reload failed. The operation is incomplete; reload the asset or Editor before further edits."));
			}
			else
			{
				Result.Errors.Add(TEXT("The Editor package recovery reload failed after the Git operation stopped. Reload the asset or Editor before further edits."));
			}
			Result.bSucceeded = false;
		}
	}

	const EGitLocalSourceControlOperationPhase TerminalPhase = !Result.bReloadSucceeded
		? EGitLocalSourceControlOperationPhase::Failed
		: Result.bCancelled ? EGitLocalSourceControlOperationPhase::Cancelled
		: Result.bSucceeded ? EGitLocalSourceControlOperationPhase::Completed : EGitLocalSourceControlOperationPhase::Failed;
	bool bInternalDeferredCleanup = false;
	TArray<FString> DeferredCleanupAssetObjectPaths;
	{
		FScopeLock Lock(&State->Mutex);
		const double NowSeconds = FPlatformTime::Seconds();
		GitLocalSourceControlPrivate::AccumulateCurrentPhaseLocked(*State, NowSeconds);
		State->Result = Result;
		State->Phase = TerminalPhase;
		State->TerminalReason = !Result.bReloadSucceeded
			? GitLocalSourceControlPrivate::EOperationTerminalReason::ReloadFailed
			: Result.bCancelled ? GitLocalSourceControlPrivate::EOperationTerminalReason::Cancelled
			: Result.bSucceeded ? GitLocalSourceControlPrivate::EOperationTerminalReason::Completed : GitLocalSourceControlPrivate::EOperationTerminalReason::WorkerFailed;
		State->AffectedFileCount = Result.AffectedFiles.Num();
		State->HistoryEntryCount = Result.History.Num();
		State->TerminalPublishedSeconds = NowSeconds;
		bInternalDeferredCleanup = State->bInternalDeferredCleanup;
		DeferredCleanupAssetObjectPaths = State->DeferredCleanupAssetObjectPaths;
	}
	Phase = TerminalPhase;
	bTerminal = true;
	if (bInternalDeferredCleanup && (!Result.bSucceeded || !Result.bReloadSucceeded))
	{
		const FString Diagnostic = FString::Printf(
			TEXT("Deferred Git cleanup failed for asset paths [%s]. succeeded=%s reload_succeeded=%s errors=[%s]. The original restore may remain partially applied; inspect these assets before further edits."),
			*FString::Join(DeferredCleanupAssetObjectPaths, TEXT(", ")),
			Result.bSucceeded ? TEXT("true") : TEXT("false"),
			Result.bReloadSucceeded ? TEXT("true") : TEXT("false"),
			*FString::Join(Result.Errors, TEXT(" | ")));
		UE_LOG(LogGitStandalone, Error, TEXT("%s"), *Diagnostic);
#if WITH_DEV_AUTOMATION_TESTS
		GitLocalSourceControlPrivate::SetLastDeferredCleanupDiagnostic(Diagnostic);
#endif
	}
	if (bBroadcastNotifications)
	{
		OnProgress.Broadcast(TerminalPhase);
		OnCompleted.Broadcast(Result);
	}

	if (!DeferredDiscardObjectPaths.IsEmpty())
	{
		TArray<FString> ObjectPaths = MoveTemp(DeferredDiscardObjectPaths);
		{
			FScopeLock Lock(&State->Mutex);
			State->bDeferredCleanupRequested = true;
		}
		if (bDiskMutationCommitted)
		{
			{
				FScopeLock Lock(&State->Mutex);
				State->bDeferredCleanupLaunchAttempted = true;
			}
#if WITH_DEV_AUTOMATION_TESTS
			++GitLocalSourceControlPrivate::GDeferredCleanupLaunchCount;
#endif
			GitLocalSourceControlPrivate::StartDiscardTrackedInternal(ObjectPaths, bAllowShutdownCleanup, true);
		}
	}
	GitLocalSourceControlPrivate::EmitTerminalTelemetry(State.ToSharedRef());
}

bool UGitLocalSourceControlOperation::ScheduleDiscardTrackedAfterCompletion(const TArray<FString>& AssetObjectPaths)
{
	check(IsInGameThread());
	if (!State.IsValid() || bTerminal || AssetObjectPaths.IsEmpty() || !DeferredDiscardObjectPaths.IsEmpty())
	{
		return false;
	}
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bCanScheduleDeferredDiscard)
		{
			return false;
		}
	}
	DeferredDiscardObjectPaths = AssetObjectPaths;
	{
		FScopeLock Lock(&State->Mutex);
		State->bDeferredCleanupRequested = true;
	}
	return true;
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
		return Phase;
	}
	FScopeLock Lock(&State->Mutex);
	return State->Phase;
}

FGitLocalSourceControlOperationResult UGitLocalSourceControlOperation::GetResult() const
{
	check(IsInGameThread());
	return Result;
}

FGitLocalSourceControlProviderInfo UGitLocalSourceControlLibrary::GetProviderInfo(const FString& AssetObjectPath)
{
	return GitLocalSourceControlPrivate::GetProviderInfoOnGameThread(AssetObjectPath);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartLoadHistory(const FString& AssetObjectPath, const EGitLocalSourceControlHistoryMode Mode)
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::FResolvedTarget Target;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTarget(AssetObjectPath, GitLocalSourceControlPrivate::ETargetAccess::ReadOnly, Target, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error, GitLocalSourceControlPrivate::EOperationTelemetryKind::LoadHistory, 1);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
		GitLocalSourceControlPrivate::EOperationTelemetryKind::LoadHistory, 1);
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."), GitLocalSourceControlPrivate::EOperationTelemetryKind::LoadHistory, 1);
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Mode]() mutable
	{
		GitLocalSourceControlPrivate::MarkWorkerStarted(State);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		FString ResolveError;
		if (!GitLocalSourceControlPrivate::ResolveRepositoryForTarget(Target, ResolveError))
		{
			Result.Errors.Add(ResolveError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		FString CapturedHead;
		bool bHeadChanged = false;
		if (GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false, Mode, CapturedHead, bHeadChanged, Errors, History))
		{
			Result.bSucceeded = true;
			GitLocalSourceControlPrivate::AddHistory(Result, History);
			if (bHeadChanged)
			{
				Result.Errors.Add(TEXT("Repository HEAD changed while history was loading. Reload history to refresh."));
			}
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
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error, GitLocalSourceControlPrivate::EOperationTelemetryKind::FetchLfsRevision, 1);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
		GitLocalSourceControlPrivate::EOperationTelemetryKind::FetchLfsRevision, 1);
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."), GitLocalSourceControlPrivate::EOperationTelemetryKind::FetchLfsRevision, 1);
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]() mutable
	{
		GitLocalSourceControlPrivate::MarkWorkerStarted(State);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		FString ResolveError;
		if (!GitLocalSourceControlPrivate::ResolveRepositoryForTarget(Target, ResolveError))
		{
			Result.Errors.Add(ResolveError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		FString CapturedHead;
		bool bHeadChanged = false;
		if (!GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false,
			EGitLocalSourceControlHistoryMode::ExactRenames, CapturedHead, bHeadChanged, Errors, History))
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
		const FString MaterializedFilename = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("git-source-control-lfs-verify-"), TEXT(".tmp"));
		IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
		Result.bSucceeded = SelectedRevision.ExportToFile(MaterializedFilename);
		IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
		if (!Result.bSucceeded)
		{
			Result.Errors.Add(TEXT("Could not verify and materialize the selected revision's Git LFS content."));
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
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error, GitLocalSourceControlPrivate::EOperationTelemetryKind::RestoreRevision, 1);
	}
	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
		GitLocalSourceControlPrivate::EOperationTelemetryKind::RestoreRevision, 1);
	State->bCanScheduleDeferredDiscard = true;
	if (!GitLocalSourceControlPrivate::PrepareLoadedPackagesForMutation({ Target }, State, true, Error))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error, GitLocalSourceControlPrivate::EOperationTelemetryKind::RestoreRevision, 1);
	}
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."), GitLocalSourceControlPrivate::EOperationTelemetryKind::RestoreRevision, 1);
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]() mutable
	{
		GitLocalSourceControlPrivate::MarkWorkerStarted(State);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		FString ResolveError;
		if (!GitLocalSourceControlPrivate::ResolveRepositoryForTarget(Target, ResolveError))
		{
			Result.Errors.Add(ResolveError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		FString CapturedHead;
		bool bHeadChanged = false;
		if (!GitSourceControlUtils::RunGetHistory(Target.GitBinary, Target.RepositoryRoot, Target.Filename, false,
			EGitLocalSourceControlHistoryMode::ExactRenames, CapturedHead, bHeadChanged, Errors, History))
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
		GitSourceControlAssetOperations::FGitSourceControlAssetOperations Operations(Target.GitBinary, Target.RepositoryRoot);
		GitSourceControlAssetOperations::FGitAssetOperationResult AssetResult;
		const bool bRestored = Operations.RestoreRevisionToWorkspace(Target.Filename, SelectedRevision.CommitId, SelectedRevision.Filename,
			GitLocalSourceControlPrivate::MakePreauthorizedClosedAssetCallbacks(State), AssetResult);
		if (bRestored && AssetResult.bSucceeded)
		{
			GitLocalSourceControlPrivate::MarkDiskMutationCommitted(State);
		}
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bRestored && AssetResult.bSucceeded;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State);
}

UGitLocalSourceControlOperation* GitLocalSourceControlPrivate::StartDiscardTrackedInternal(const TArray<FString>& AssetObjectPaths,
	const bool bAllowDuringShutdown, const bool bInternalDeferredCleanup)
{
	check(IsInGameThread());
	TArray<GitLocalSourceControlPrivate::FResolvedTarget> Targets;
	FString Error;
	if (!GitLocalSourceControlPrivate::ResolveTargets(AssetObjectPaths, GitLocalSourceControlPrivate::ETargetAccess::Mutation, Targets, Error))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error,
			bInternalDeferredCleanup ? GitLocalSourceControlPrivate::EOperationTelemetryKind::DeferredDiscardTracked : GitLocalSourceControlPrivate::EOperationTelemetryKind::DiscardTracked,
			AssetObjectPaths.Num(), bAllowDuringShutdown, bInternalDeferredCleanup, AssetObjectPaths);
	}
	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
		bInternalDeferredCleanup ? GitLocalSourceControlPrivate::EOperationTelemetryKind::DeferredDiscardTracked : GitLocalSourceControlPrivate::EOperationTelemetryKind::DiscardTracked,
		AssetObjectPaths.Num());
	State->bCanScheduleDeferredDiscard = true;
	State->bInternalDeferredCleanup = bInternalDeferredCleanup;
	State->DeferredCleanupAssetObjectPaths = AssetObjectPaths;
	if (!GitLocalSourceControlPrivate::PrepareLoadedPackagesForMutation(Targets, State, false, Error))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error,
			bInternalDeferredCleanup ? GitLocalSourceControlPrivate::EOperationTelemetryKind::DeferredDiscardTracked : GitLocalSourceControlPrivate::EOperationTelemetryKind::DiscardTracked,
			AssetObjectPaths.Num(), bAllowDuringShutdown, bInternalDeferredCleanup, AssetObjectPaths);
	}
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State, bAllowDuringShutdown))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."),
			bInternalDeferredCleanup ? GitLocalSourceControlPrivate::EOperationTelemetryKind::DeferredDiscardTracked : GitLocalSourceControlPrivate::EOperationTelemetryKind::DiscardTracked,
			AssetObjectPaths.Num(), bAllowDuringShutdown, bInternalDeferredCleanup, AssetObjectPaths);
	}
	Async(EAsyncExecution::ThreadPool, [State, Targets]() mutable
	{
		GitLocalSourceControlPrivate::MarkWorkerStarted(State);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::Preparing, true);
		FGitLocalSourceControlOperationResult Result;
		FString ResolveError;
		if (!GitLocalSourceControlPrivate::ResolveRepositoriesForTargets(Targets, ResolveError))
		{
			Result.Errors.Add(ResolveError);
			GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
			return;
		}
		TArray<FString> Files;
		for (const GitLocalSourceControlPrivate::FResolvedTarget& Target : Targets)
		{
			Files.Add(Target.Filename);
		}
		GitSourceControlAssetOperations::FGitSourceControlAssetOperations Operations(Targets[0].GitBinary, Targets[0].RepositoryRoot);
		GitSourceControlAssetOperations::FGitAssetOperationResult AssetResult;
		const bool bDiscarded = Operations.DiscardTrackedFiles(Files, GitLocalSourceControlPrivate::MakePreauthorizedClosedAssetCallbacks(State), AssetResult);
		if (bDiscarded && AssetResult.bSucceeded)
		{
			GitLocalSourceControlPrivate::MarkDiskMutationCommitted(State);
		}
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bDiscarded && AssetResult.bSucceeded;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	return GitLocalSourceControlPrivate::MakeOperation(State, bAllowDuringShutdown);
}

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartDiscardTracked(const TArray<FString>& AssetObjectPaths)
{
	return GitLocalSourceControlPrivate::StartDiscardTrackedInternal(AssetObjectPaths, false, false);
}

void GitLocalSourceControl::ShutdownOperations()
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::GetOperationRegistry().BeginShutdown();
	GitLocalSourceControlPrivate::GetOperationManager().Shutdown();
}

void GitLocalSourceControl::StartupOperations()
{
	check(IsInGameThread());
	GitLocalSourceControlPrivate::GetOperationManager().Startup();
}

#if WITH_DEV_AUTOMATION_TESTS

void GitLocalSourceControl::Testing::PumpOperations()
{
	GitLocalSourceControlPrivate::GetOperationManager().PumpForTesting();
}

int32 GitLocalSourceControl::Testing::GetManagedOperationCount()
{
	return GitLocalSourceControlPrivate::GetOperationManager().GetManagedOperationCount();
}

bool GitLocalSourceControl::Testing::HasOperationTicker()
{
	return GitLocalSourceControlPrivate::GetOperationManager().HasTicker();
}

void GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(const bool bEnabled)
{
	GitLocalSourceControlPrivate::SetForcePreparedPackageReloadFailureForTesting(bEnabled);
}

void GitLocalSourceControl::Testing::SetForceRestoreWorktreeRollback(const bool bEnabled)
{
	GitLocalSourceControlPrivate::SetForceRestoreWorktreeRollbackForTesting(bEnabled);
}

FString GitLocalSourceControl::Testing::GetLastDeferredCleanupDiagnostic()
{
	return GitLocalSourceControlPrivate::GLastDeferredCleanupDiagnostic;
}

void GitLocalSourceControl::Testing::ClearLastDeferredCleanupDiagnostic()
{
	GitLocalSourceControlPrivate::SetLastDeferredCleanupDiagnostic(FString());
}

void GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount()
{
	GitLocalSourceControlPrivate::GDeferredCleanupLaunchCount.Store(0);
}

int32 GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount()
{
	return GitLocalSourceControlPrivate::GDeferredCleanupLaunchCount.Load();
}

UGitLocalSourceControlOperation* GitLocalSourceControl::Testing::StartBlockedReadOnlyOperationForTesting()
{
	check(IsInGameThread());
	if (GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate.IsValid())
	{
		return nullptr;
	}
	const TSharedRef<GitLocalSourceControlPrivate::FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> Gate =
		MakeShared<GitLocalSourceControlPrivate::FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe>();
	GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate = Gate;
	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>(
		GitLocalSourceControlPrivate::EOperationTelemetryKind::TestOnly, 0);
	State->FinalPublicationGate = Gate;
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate.Reset();
		return nullptr;
	}
	Async(EAsyncExecution::ThreadPool, [State, Gate]()
	{
		GitLocalSourceControlPrivate::MarkWorkerStarted(State);
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(State->CancellationContext);
		GitLocalSourceControlPrivate::SetWorkerPhase(State, EGitLocalSourceControlOperationPhase::LoadingHistory, true);
		FGitLocalSourceControlOperationResult Result;
		Result.bSucceeded = true;
		GitLocalSourceControlPrivate::FinishWorker(State, MoveTemp(Result));
	});
	UGitLocalSourceControlOperation* Operation = GitLocalSourceControlPrivate::MakeOperation(State);
	if (Operation == nullptr)
	{
		Gate->ReleaseFinalPublication->Trigger();
		GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate.Reset();
	}
	return Operation;
}

bool GitLocalSourceControl::Testing::WaitForBlockedReadOnlyOperationToReachFinalPublication(const double TimeoutSeconds)
{
	check(IsInGameThread());
	const TSharedPtr<GitLocalSourceControlPrivate::FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> Gate =
		GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate;
	return Gate.IsValid() && Gate->WorkerReachedFinalPublication->Wait(FTimespan::FromSeconds(TimeoutSeconds));
}

void GitLocalSourceControl::Testing::ReleaseBlockedReadOnlyOperation()
{
	check(IsInGameThread());
	const TSharedPtr<GitLocalSourceControlPrivate::FBlockedReadOnlyOperationGate, ESPMode::ThreadSafe> Gate =
		MoveTemp(GitLocalSourceControlPrivate::GBlockedReadOnlyOperationGate);
	if (Gate.IsValid())
	{
		Gate->ReleaseFinalPublication->Trigger();
	}
}

bool GitLocalSourceControl::Testing::DrainOperationsForTesting()
{
	return GitLocalSourceControlPrivate::GetOperationManager().DrainForTesting();
}

#endif

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
