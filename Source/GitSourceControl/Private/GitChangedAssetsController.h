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

/**
 * Game-thread owned coordinator for the Changed Assets tab.
 *
 * A controller only owns one in-flight repository snapshot.  It never polls and
 * its async work communicates back to Slate on the GameThread.
 */
class FGitChangedAssetsController final : public TSharedFromThis<FGitChangedAssetsController, ESPMode::ThreadSafe>
{
public:
	DECLARE_MULTICAST_DELEGATE(FOnChangedAssetsUpdated);

	FGitChangedAssetsController();
	~FGitChangedAssetsController();

	/** Starts a new, one-shot repository snapshot. Safe to call while a prior snapshot is in flight. */
	void Refresh(bool bClearPreviousError = true);

	/** Cancels delivery of outstanding work and releases the current snapshot. Must run on the GameThread. */
	void Shutdown();

	bool IsRefreshing() const;
	bool IsReverting() const;
	const FGitChangedAssetSnapshot* GetSnapshot() const;
	const FString& GetLastError() const;
	const FDateTime& GetLastSuccessfulRefreshTime() const;

	/**
	 * Reverts exactly the supplied entries to the snapshot's pinned HEAD.
	 * Callers must have already presented the destructive-action confirmation.
	 */
	void RevertToHead(TArray<FGitChangedAssetEntry> Entries);

	FOnChangedAssetsUpdated& OnChanged();

private:
	struct FPendingHeadMetadataApply
	{
		uint64 Generation = 0;
		TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> Snapshot;
		TArray<FGitChangedAssetHeadMetadataResult> Results;
		int32 NextResultIndex = 0;
		FString Error;
	};

	void CompleteRefresh(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot, FString Error);
	void CompleteCurrentMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot);
	void CompleteHeadMetadata(uint64 CompletedGeneration, TSharedPtr<FGitChangedAssetSnapshot, ESPMode::ThreadSafe> CompletedSnapshot,
		TArray<FGitChangedAssetHeadMetadataResult> Results, FString Error);
	void ApplyPendingHeadMetadata(uint64 PendingGeneration);
	void ClearPendingHeadMetadata();
	void CompleteOwnerFallback(uint64 CompletedGeneration);
	void CompleteRevert(bool bDiskMutationSucceeded, bool bEditorReloadSucceeded, FString ResultMessage);
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
	TOptional<FPendingHeadMetadataApply> PendingHeadMetadataApply;
	TSharedPtr<GitChangedAssetsControllerPrivate::FGameThreadDispatcher, ESPMode::ThreadSafe> GameThreadDispatcher;
	TSharedPtr<GitChangedAssetsControllerPrivate::FWorkerState, ESPMode::ThreadSafe> WorkerState;
	FOnChangedAssetsUpdated ChangedDelegate;
};
