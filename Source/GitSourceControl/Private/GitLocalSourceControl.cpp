// Copyright (c) 2026

#include "GitLocalSourceControl.h"

#include "Async/Async.h"
#include "GitSourceControlAssetOperations.h"
#include "GitSourceControlRevision.h"
#include "GitStandaloneHistory.h"
#include "GitSourceControlUtils.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "PackageTools.h"
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
	TArray<FString> ReloadFilenames;
	bool bCancellationAllowed = true;
	bool bWorkerFinished = false;
};

namespace GitLocalSourceControlPrivate
{
	bool ReloadPreparedPackages(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState);

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
			bool bNeedsReload = false;
			{
				FScopeLock StateLock(&InState->Mutex);
				bNeedsReload = !InState->ReloadFilenames.IsEmpty();
			}
			FScopeLock Lock(&Mutex);
			ActiveStates.RemoveSingleSwap(InState, EAllowShrinking::No);
			if (bNeedsReload)
			{
				if (!PendingReloadStates.ContainsByPredicate([&InState](const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& Candidate)
				{
					return &Candidate.Get() == &InState.Get();
				}))
				{
					PendingReloadStates.Add(InState);
				}
			}
			if (ActiveStates.IsEmpty())
			{
				AllWorkersFinished->Trigger();
			}
		}

		void Shutdown()
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

			while (!AllWorkersFinished->Wait(10))
			{}

			TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> StatesToReload;
			{
				FScopeLock Lock(&Mutex);
				StatesToReload = PendingReloadStates;
				PendingReloadStates.Reset();
			}
			for (const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& State : StatesToReload)
			{
				const bool bReloadSucceeded = ReloadPreparedPackages(State);
				FScopeLock StateLock(&State->Mutex);
				State->Result.bReloadSucceeded = bReloadSucceeded;
			}
		}

		void CompleteReload(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
		{
			FScopeLock Lock(&Mutex);
			PendingReloadStates.RemoveAllSwap([&InState](const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& Candidate)
			{
				return &Candidate.Get() == &InState.Get();
			}, EAllowShrinking::No);
		}

	private:
		FCriticalSection Mutex;
		FEvent* AllWorkersFinished = nullptr;
		bool bAcceptingNewWorkers = true;
		TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> ActiveStates;
		TArray<TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>> PendingReloadStates;
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

	FGitLocalSourceControlProviderInfo GetProviderInfoOnGameThread()
	{
		check(IsInGameThread());
		FGitLocalSourceControlProviderInfo Info;
		Info.bAvailable = GitSourceControlUtils::ResolveStandaloneRepositoryForFile(FPaths::ProjectDir(), Info.GitBinary, Info.RepositoryRoot, Info.Error);
		if (!Info.bAvailable && Info.Error.IsEmpty())
		{
			Info.Error = TEXT("Could not resolve a local Git executable and repository root.");
		}
		return Info;
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

	bool ReloadPreparedPackages(const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>& InState)
	{
		check(IsInGameThread());
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
				return false;
			}
			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr || Package->FindAssetInPackage() == nullptr)
			{
				return false;
			}
		}
		return true;
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

	bool bNeedsReload = false;
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bWorkerFinished)
		{
			return;
		}
		Result = State->Result;
		bNeedsReload = !State->ReloadFilenames.IsEmpty();
	}
	if (bNeedsReload)
	{
		Result.bReloadSucceeded = GitLocalSourceControlPrivate::ReloadPreparedPackages(State.ToSharedRef());
		GitLocalSourceControlPrivate::GetOperationRegistry().CompleteReload(State.ToSharedRef());
	}
	{
		FScopeLock Lock(&State->Mutex);
		State->Result.bReloadSucceeded = Result.bReloadSucceeded;
		CompletedPhase = State->Phase;
	}
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

UGitLocalSourceControlOperation* UGitLocalSourceControlLibrary::StartLoadHistory(const FString& AssetObjectPath, const EGitLocalSourceControlHistoryMode Mode)
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
	Async(EAsyncExecution::ThreadPool, [State, Target, Mode]() mutable
	{
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
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}

	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]() mutable
	{
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
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}
	const TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State = MakeShared<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe>();
	if (!GitLocalSourceControlPrivate::PrepareLoadedPackagesForMutation({ Target }, State, true, Error))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Target, Revision]() mutable
	{
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
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bRestored && AssetResult.bSucceeded;
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
	if (!GitLocalSourceControlPrivate::PrepareLoadedPackagesForMutation(Targets, State, false, Error))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(Error);
	}
	if (!GitLocalSourceControlPrivate::GetOperationRegistry().TryBegin(State))
	{
		GitLocalSourceControlPrivate::ReloadPreparedPackages(State);
		return GitLocalSourceControlPrivate::MakeFailedOperation(TEXT("Git Local SourceControl is shutting down."));
	}
	Async(EAsyncExecution::ThreadPool, [State, Targets]() mutable
	{
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
		GitLocalSourceControlPrivate::AddAssetOperationResult(Result, AssetResult);
		Result.bSucceeded = bDiscarded && AssetResult.bSucceeded;
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
