// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "GitChangedAssetsMetadata.h"
#include "GitChangedAssetsModel.h"

namespace GitChangedAssetsControllerPrivate
{
	class FGameThreadDispatcher;
	class FWorkerState;
}

enum class EGitChangedAssetsRefreshPhase : uint8
{
	Idle,
	GitStatus,
	CurrentMetadata,
	HeadMetadata,
	OwnerFallback,
};

/**
 * Game-thread owned coordinator for the Changed Assets tab.
 *
 * A controller only owns one in-flight repository snapshot.  It never polls and
 * its async work communicates back to Slate on the GameThread.
 */
class FGitChangedAssetsController final : public TSharedFromThis<FGitChangedAssetsController, ESPMode::ThreadSafe>
{
public:
	/** Snapshot rows changed. This never fires for phase/progress-only activity. */
	DECLARE_MULTICAST_DELEGATE(FOnChangedAssetsRowsUpdated);
	/** Refresh/revert activity changed; consumers update status and actions without reconciling rows. */
	DECLARE_MULTICAST_DELEGATE(FOnChangedAssetsActivityUpdated);

	FGitChangedAssetsController();
	~FGitChangedAssetsController();

	/** Starts a new, one-shot repository snapshot. Safe to call while a prior snapshot is in flight. */
	void Refresh(bool bClearPreviousError = true);

	/** Cancels delivery of outstanding work and releases the current snapshot. Must run on the GameThread. */
	void Shutdown();

	/** Notify bound panels that the frozen startup Git capability reached its terminal state. */
	void HandleStartupGitCapabilityChanged();

	bool IsRefreshing() const;
	bool IsReverting() const;
	EGitChangedAssetsRefreshPhase GetRefreshPhase() const;
	int32 GetRefreshProgressCompleted() const;
	int32 GetRefreshProgressTotal() const;
	const FGitChangedAssetSnapshot* GetSnapshot() const;
	const FString& GetLastError() const;
	const FDateTime& GetLastSuccessfulRefreshTime() const;

	/**
	 * Reverts exactly the supplied entries to the snapshot's pinned HEAD.
	 * Callers must have already presented the destructive-action confirmation.
	 */
	void RevertToHead(TArray<FGitChangedAssetEntry> Entries);

	FOnChangedAssetsRowsUpdated& OnRowsChanged();
	FOnChangedAssetsActivityUpdated& OnActivityChanged();

private:
	struct FPendingCurrentMetadataApply
	{
		uint64 Generation = 0;
		TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> Snapshot;
		int32 NextEntryIndex = 0;
	};

	struct FPendingHeadMetadataApply
	{
		uint64 Generation = 0;
		TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> Snapshot;
		TArray<FGitChangedAssetHeadMetadataResult> Results;
		int32 NextResultIndex = 0;
		FString Error;
	};

	struct FPendingOwnerFallback
	{
		uint64 Generation = 0;
		TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> Snapshot;
		FString Error;
	};

	void CompleteRefresh(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error);
	void BeginCurrentMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot);
	void ApplyPendingCurrentMetadata(uint64 PendingGeneration);
	void CompleteCurrentMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot);
	void BeginHeadMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot);
	void CompleteHeadMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot,
		TArray<FGitChangedAssetHeadMetadataResult> Results, FString Error);
	void ApplyPendingHeadMetadata(uint64 PendingGeneration);
	void ClearPendingHeadMetadata();
	void BeginOwnerFallback(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString ExistingError);
	void CompleteOwnerFallback(uint64 CompletedGeneration);
	void FinishRefresh(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error);
	void FailRefresh(uint64 CompletedGeneration, FString Error);
	void ClearPendingRefreshWork();
	void SetRefreshActivity(EGitChangedAssetsRefreshPhase InPhase, int32 InCompleted, int32 InTotal);
	void CompleteRevert(bool bDiskMutationSucceeded, bool bEditorReloadSucceeded, FString ResultMessage, TArray<FString> AffectedFiles);
	void RefreshPostRevertAffectedFiles(TArray<FString> AffectedFiles, bool bPreserveLastError);
	void CompletePostRevertAffectedFilesRefresh(uint64 CompletedGeneration, TArray<FString> AffectedFiles,
		TArray<FGitChangedAssetEntry> ExactStatusEntries, double ExactStatusDurationSeconds, FString Error);
	bool RemoveRevertedEntriesFromSnapshot(const TArray<FString>& AffectedFiles, FString& OutError);
	void FallBackToFullPostRevertRefresh(uint64 CompletedGeneration, bool bPreserveLastError, FString Reason);
	bool ConfirmRevert(const TArray<FGitChangedAssetEntry>& Entries, FString& OutError);

	TOptional<FGitChangedAssetSnapshot> Snapshot;
	FString LastError;
	FDateTime LastSuccessfulRefreshTime;
	uint64 Generation = 0;
	bool bRefreshing = false;
	bool bReverting = false;
	bool bPreserveLastErrorForRefresh = false;
	double PostRevertStatusRefreshStartSeconds = 0.0;
	TAtomic<bool> bShuttingDown = false;
	EGitChangedAssetsRefreshPhase RefreshPhase = EGitChangedAssetsRefreshPhase::Idle;
	int32 RefreshProgressCompleted = 0;
	int32 RefreshProgressTotal = 0;
	TOptional<FPendingCurrentMetadataApply> PendingCurrentMetadataApply;
	TOptional<FPendingHeadMetadataApply> PendingHeadMetadataApply;
	TOptional<FPendingOwnerFallback> PendingOwnerFallback;
	TSharedPtr<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> GameThreadDispatcher;
	TSharedPtr<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> WorkerState;
	FOnChangedAssetsRowsUpdated RowsChangedDelegate;
	FOnChangedAssetsActivityUpdated ActivityChangedDelegate;
};
