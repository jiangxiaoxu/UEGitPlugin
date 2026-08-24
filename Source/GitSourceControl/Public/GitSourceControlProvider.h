// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "ISourceControlProvider.h"
#include "IGitSourceControlWorker.h"
#include "Runtime/Launch/Resources/Version.h"

class FGitSourceControlState;
struct FFileChangeData;

class FGitSourceControlCommand;

DECLARE_DELEGATE_RetVal(FGitSourceControlWorkerRef, FGetGitSourceControlWorker)

/// Git version and capabilites extracted from the string "git version 2.11.0.windows.3"
struct FGitVersion
{
	// Git version extracted from the string "git version 2.11.0.windows.3" (Windows), "git version 2.11.0" (Linux/Mac/Cygwin/WSL) or "git version 2.31.1.vfs.0.3" (Microsoft)
	int Major;   // 2	Major version number
	int Minor;   // 31	Minor version number
	int Patch;   // 1	Patch/bugfix number
	bool bIsFork;
	FString Fork; // "vfs"
	int ForkMajor; // 0	Fork specific revision number
	int ForkMinor; // 3 
	int ForkPatch; // ?

	FGitVersion() 
		: Major(0)
		, Minor(0)
		, Patch(0)
		, bIsFork(false)
		, ForkMajor(0)
		, ForkMinor(0)
		, ForkPatch(0)
	{
	}
};

class GITSOURCECONTROL_API FGitSourceControlProvider final : public ISourceControlProvider
{
public:
	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName(void) const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override;
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn) override;
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override;
	virtual ECommandResult::Type GetState( const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage ) override;
#if ENGINE_MAJOR_VERSION >= 5
    virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
#endif
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
#if ENGINE_MAJOR_VERSION < 5
	virtual ECommandResult::Type Execute( const FSourceControlOperationRef& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual void CancelOperation( const FSourceControlOperationRef& InOperation ) override;
#else
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual void CancelOperation( const FSourceControlOperationRef& InOperation ) override;
#endif
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesCheckout() const override;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	virtual bool UsesFileRevisions() const override;
	virtual TOptional<bool> IsAtLatestRevision() const override;
	virtual TOptional<int> GetNumLocalChanges() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	virtual bool AllowsDiffAgainstDepot() const override;
	virtual bool UsesUncontrolledChangelists() const override;
	virtual bool UsesSnapshots() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	virtual bool CanExecuteOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
#endif
	virtual void Tick() override;
	virtual TArray< TSharedRef<class ISourceControlLabel> > GetLabels( const FString& InMatchingSpec ) const override;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override;
#endif	

#if ENGINE_MAJOR_VERSION >= 5
	virtual TArray<FSourceControlChangelistRef> GetChangelists( EStateCacheUsage::Type InStateCacheUsage ) override;
#endif

#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	/** 自动发现 Git binary 并验证其可用性. */
	void CheckGitAvailability();

	/**
	 * Find the .git/ repository and validate the local Git capability.
	 */
	void CheckRepositoryStatus();

	/** Is git binary found and working. */
	inline bool IsGitAvailable() const
	{
		return bGitAvailable;
	}

	/** Git version for feature checking */
	inline const FGitVersion& GetGitVersion() const
	{
		return GitVersion;
	}

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	inline const FString& GetPathToRepositoryRoot() const
	{
		return PathToRepositoryRoot;
	}

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	inline const FString& GetPathToGitRoot() const
	{
		return PathToGitRoot;
	}

	/** Gets the path to the Git binary */
	inline const FString& GetGitBinaryPath() const
	{
		return PathToGitBinary;
	}

	/** 返回文件所在的 canonical Git root; 不在已连接仓库内时返回空字符串. */
	FString ResolveRepositoryRootForFile(const FString& Filename) const;

	/** Helper function used to update state cache */
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& Filename);
	
	/**
	 * Register a worker with the provider.
	 * This is used internally so the provider can maintain a map of all available operations.
	 */
	void RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate );

	/** Set list of error messages that occurred after last perforce command */
	void SetLastErrors(const TArray<FText>& InErrors);

	/** Get list of error messages that occurred after last perforce command */
	TArray<FText> GetLastErrors() const;

	/** Get number of error messages seen after running last perforce command */
	int32 GetNumLastErrors() const;

	/** Remove a named file from the state cache */
	bool RemoveFileFromCache(const FString& Filename);

	/** Get files in cache */
	TArray<FString> GetFilesInCache();

private:
	struct FDirectoryWatch
	{
		FString Directory;
		FString RepositoryRoot;
		FDelegateHandle Handle;
	};

	void RegisterDirectoryWatchers();
	void RegisterDirectoryWatchesForRoot(const FString& RepositoryRoot, bool bWatchProjectManagedPaths);
	void UnregisterDirectoryWatchers();
	void OnDirectoryChanged(const TArray<FFileChangeData>& FileChanges, const FString& RepositoryRoot, bool bRepositoryMetadata);
	bool ApplyPendingDirectoryChanges();
	void QueueStatusRefresh(const TArray<FString>& Filenames);
	void IssuePendingStatusRefreshes();

	/** Is git binary found and working. */
	bool bGitAvailable = false;

	/** Is git repository found. */
	bool bGitRepositoryFound = false;

	FString PathToGitBinary;

	/** Critical section for thread safety of error messages that occurred after last perforce command */
	mutable FCriticalSection LastErrorsCriticalSection;

	/** List of error messages that occurred after last perforce command */
	TArray<FText> LastErrors;

	/** 仅在命令完成后的 game thread 修改状态缓存. */
	mutable FCriticalSection StateCacheCriticalSection;

	/** Helper function for Execute() */
	TSharedPtr<class IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;

	/** Helper function for running command synchronously. */
	ECommandResult::Type ExecuteSynchronousCommand(class FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg);
	/** Issue a command asynchronously if possible. */
	ECommandResult::Type IssueCommand(class FGitSourceControlCommand& InCommand, const bool bSynchronous = false );

	/** Output any messages this command holds */
	void OutputCommandMessages(const class FGitSourceControlCommand& InCommand) const;

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	FString PathToRepositoryRoot;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToGitRoot;

	/** State cache */
	TMap<FString, TSharedRef<class FGitSourceControlState, ESPMode::ThreadSafe> > StateCache;

	/** The currently registered revision control operations */
	TMap<FName, FGetGitSourceControlWorker> WorkersMap;

	/** Queue for commands given by the main thread */
	TArray < FGitSourceControlCommand* > CommandQueue;

	/** Repository generation observed when each queued read command started. */
	TMap<FGitSourceControlCommand*, uint64> CommandGenerations;

	/** Provider-owned local filesystem watches. */
	TArray<FDirectoryWatch> DirectoryWatches;
	TMap<FString, TSet<FString>> PendingChangedPathsByRepository;
	TSet<FString> PendingRepositoryMetadataInvalidations;
	TMap<FString, TSet<FString>> PendingStatusRefreshesByRepository;
	double PendingWatchInvalidationTime = 0.0;

	/** For notifying when the revision control states in the cache have changed */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Git version for feature checking */
	FGitVersion GitVersion;

};
