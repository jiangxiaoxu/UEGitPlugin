// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlProvider.h"

#include "GitMessageLog.h"
#include "GitSourceControlState.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/QueuedThreadPool.h"
#include "GitSourceControlCommand.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "SGitSourceControlSettings.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Logging/MessageLog.h"
#include "ScopedSourceControlProgress.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/MessageDialog.h"

#include "Runtime/Launch/Resources/Version.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

static FName ProviderName("Git");

namespace GitSourceControlProviderPrivate
{
FString NormalizeCacheFilename(const FString& Filename)
{
	FString Result = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::CollapseRelativeDirectories(Result);
	FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
	Result.ToLowerInline();
#endif
	return Result;
}

FString NormalizeDisplayFilename(const FString& Filename)
{
	FString Result = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::CollapseRelativeDirectories(Result);
	FPaths::NormalizeFilename(Result);

	// Preserve the actual on-disk spelling where the path exists. Cache keys stay
	// case-insensitive on Windows, but SourceControl revisions must retain the
	// spelling UE and Git use when opening the asset.
	if (FPaths::FileExists(Result) || FPaths::DirectoryExists(Result))
	{
		const FString OnDiskFilename = IFileManager::Get().GetFilenameOnDisk(*Result);
		if (!OnDiskFilename.IsEmpty())
		{
			Result = OnDiskFilename;
			FPaths::NormalizeFilename(Result);
		}
	}

	return Result;
}

FString ResolveNearestRepositoryRoot(const FString& Filename, const FString& FallbackRepositoryRoot)
{
	const FString CanonicalFallbackRoot = NormalizeCacheFilename(FallbackRepositoryRoot);
	FString Candidate = FPaths::GetPath(NormalizeCacheFilename(Filename));
	while (!Candidate.IsEmpty() && (Candidate == CanonicalFallbackRoot || FPaths::IsUnderDirectory(Candidate, CanonicalFallbackRoot)))
	{
		const FString GitPath = FPaths::Combine(Candidate, TEXT(".git"));
		if (FPaths::FileExists(GitPath) || FPaths::DirectoryExists(GitPath))
		{
			return Candidate;
		}
		if (Candidate == CanonicalFallbackRoot)
		{
			break;
		}
		Candidate = FPaths::GetPath(Candidate);
	}
	return FString();
}

TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> CloneUpdateStatusOperation(const FUpdateStatus& SourceOperation)
{
	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
	Operation->SetUpdateHistory(SourceOperation.ShouldUpdateHistory());
	Operation->SetGetOpenedOnly(SourceOperation.ShouldGetOpenedOnly());
	Operation->SetUpdateModifiedState(SourceOperation.ShouldUpdateModifiedState());
	Operation->SetUpdateModifiedStateToLocalRevision(SourceOperation.ShouldUpdateModifiedStateToLocalRevision());
	Operation->SetCheckingAllFiles(SourceOperation.ShouldCheckAllFiles());
	Operation->SetQuiet(SourceOperation.ShouldBeQuiet());
	Operation->SetForceUpdate(SourceOperation.ShouldForceUpdate());
	return Operation;
}

struct FUpdateStatusBatch
{
	FUpdateStatusBatch(const FSourceControlOperationRef& InOperation, const FSourceControlOperationComplete& InCompletionDelegate, const int32 InRemaining)
		: Operation(InOperation)
		, CompletionDelegate(InCompletionDelegate)
		, Remaining(InRemaining)
	{
	}

	FSourceControlOperationRef Operation;
	FSourceControlOperationComplete CompletionDelegate;
	int32 Remaining;
	ECommandResult::Type Result = ECommandResult::Succeeded;
};
}

void FGitSourceControlProvider::Init(bool bForceConnection)
{
	(void)bForceConnection;
	// Editor 启动期间可能重复调用 Init().
	if (!bGitAvailable)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
		if(Plugin.IsValid())
		{
			UE_LOG(LogSourceControl, Log, TEXT("Git plugin '%s'"), *(Plugin->GetDescriptor().VersionName));
		}

		CheckGitAvailability();
	}
}

void FGitSourceControlProvider::CheckGitAvailability()
{
	PathToGitBinary = GitSourceControlUtils::FindGitBinaryPath();

	if (!PathToGitBinary.IsEmpty() && GitSourceControlUtils::CheckGitAvailability(PathToGitBinary, &GitVersion))
	{
		UE_LOG(LogSourceControl, Log, TEXT("Using '%s'"), *PathToGitBinary);
		bGitAvailable = true;
		CheckRepositoryStatus();
	}
	else
	{
		UnregisterDirectoryWatchers();
		bGitAvailable = false;
		bGitRepositoryFound = false;
		PathToGitRoot.Reset();
		PathToRepositoryRoot.Reset();
		GitVersion = FGitVersion();
	}
}

void FGitSourceControlProvider::CheckRepositoryStatus()
{
	UnregisterDirectoryWatchers();
	bGitRepositoryFound = false;
	PathToGitRoot.Reset();
	PathToRepositoryRoot.Reset();

	if (!bGitAvailable)
	{
		return;
	}

	// 查找包含当前项目的 Git root, 不访问 remote.
	const FString PathToProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (!GitSourceControlUtils::FindRootDirectory(PathToProjectDir, PathToGitRoot))
	{
		UE_LOG(LogSourceControl, Log, TEXT("No Git repository contains '%s'."), *PathToProjectDir);
		return;
	}
	PathToRepositoryRoot = PathToGitRoot;

	FString CapabilityError;
	if (!GitSourceControlUtils::CheckLocalGitCapabilities(PathToGitBinary, PathToRepositoryRoot, CapabilityError))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Git is missing required local status capability: %s"), *CapabilityError);
		PathToGitRoot.Reset();
		PathToRepositoryRoot.Reset();
		return;
	}

	bGitRepositoryFound = true;
	RegisterDirectoryWatchers();
}

void FGitSourceControlProvider::RegisterDirectoryWatchers()
{
	RegisterDirectoryWatchesForRoot(PathToGitRoot, true);
}

void FGitSourceControlProvider::RegisterDirectoryWatchesForRoot(const FString& RepositoryRoot, const bool bWatchProjectManagedPaths)
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	const FString CanonicalRepositoryRoot = GitSourceControlProviderPrivate::NormalizeCacheFilename(RepositoryRoot);
	if (CanonicalRepositoryRoot.IsEmpty() || DirectoryWatches.ContainsByPredicate([&CanonicalRepositoryRoot](const FDirectoryWatch& Watch)
	{
		return Watch.RepositoryRoot == CanonicalRepositoryRoot;
	}))
	{
		return;
	}

	FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get();
	if (DirectoryWatcher == nullptr)
	{
		return;
	}

	auto RegisterWatch = [this, DirectoryWatcher, &CanonicalRepositoryRoot](const FString& Directory, const bool bRepositoryMetadata, const uint32 WatchOptions)
	{
		const FString CanonicalDirectory = GitSourceControlProviderPrivate::NormalizeCacheFilename(Directory);
		if (!FPaths::DirectoryExists(CanonicalDirectory))
		{
			return;
		}

		FDelegateHandle Handle;
		if (DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
			CanonicalDirectory,
			IDirectoryWatcher::FDirectoryChanged::CreateLambda([this, RepositoryRoot = CanonicalRepositoryRoot, bRepositoryMetadata](const TArray<FFileChangeData>& FileChanges)
			{
				OnDirectoryChanged(FileChanges, RepositoryRoot, bRepositoryMetadata);
			}),
			Handle,
			WatchOptions))
		{
			FDirectoryWatch& Watch = DirectoryWatches.AddDefaulted_GetRef();
			Watch.Directory = CanonicalDirectory;
			Watch.RepositoryRoot = CanonicalRepositoryRoot;
			Watch.Handle = Handle;
		}
	};

	RegisterWatch(CanonicalRepositoryRoot, false, bWatchProjectManagedPaths
		? IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges | IDirectoryWatcher::WatchOptions::IgnoreChangesInSubtree
		: IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
	if (bWatchProjectManagedPaths)
	{
		RegisterWatch(FPaths::ProjectContentDir(), false, IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
		RegisterWatch(FPaths::ProjectConfigDir(), false, IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
		RegisterWatch(FPaths::ProjectPluginsDir(), false, IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
	}

	TArray<FString> GitMetadataPaths;
	TArray<FString> Results;
	TArray<FString> Errors;
	if (GitSourceControlUtils::RunCommand(
		TEXT("rev-parse"),
		PathToGitBinary,
		CanonicalRepositoryRoot,
		{ TEXT("--git-dir"), TEXT("--git-common-dir") },
		FGitSourceControlModule::GetEmptyStringArray(),
		Results,
		Errors))
	{
		for (FString GitMetadataPath : Results)
		{
			if (FPaths::IsRelative(GitMetadataPath))
			{
				GitMetadataPath = FPaths::Combine(CanonicalRepositoryRoot, GitMetadataPath);
			}
			GitMetadataPaths.AddUnique(MoveTemp(GitMetadataPath));
		}
	}

	for (const FString& GitMetadataPath : GitMetadataPaths)
	{
		RegisterWatch(GitMetadataPath, true, IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
	}
}

void FGitSourceControlProvider::UnregisterDirectoryWatchers()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get())
		{
			for (const FDirectoryWatch& Watch : DirectoryWatches)
			{
				DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(Watch.Directory, Watch.Handle);
			}
		}
	}

	DirectoryWatches.Empty();
	PendingChangedPathsByRepository.Empty();
	PendingRepositoryMetadataInvalidations.Empty();
	PendingStatusRefreshesByRepository.Empty();
	PendingWatchInvalidationTime = 0.0;
}

void FGitSourceControlProvider::OnDirectoryChanged(const TArray<FFileChangeData>& FileChanges, const FString& RepositoryRoot, const bool bRepositoryMetadata)
{
	const FString CanonicalRepositoryRoot = GitSourceControlProviderPrivate::NormalizeCacheFilename(RepositoryRoot);
	for (const FFileChangeData& FileChange : FileChanges)
	{
		if (FileChange.Action == FFileChangeData::FCA_RescanRequired || FileChange.Filename.IsEmpty())
		{
			PendingRepositoryMetadataInvalidations.Add(CanonicalRepositoryRoot);
			continue;
		}

		const FString Filename = GitSourceControlProviderPrivate::NormalizeCacheFilename(FileChange.Filename);
		const FString CleanFilename = FPaths::GetCleanFilename(Filename);
		if (CleanFilename.StartsWith(TEXT(".git-source-control-txn-"), ESearchCase::IgnoreCase)
			|| CleanFilename.StartsWith(TEXT(".uegit-backup-"), ESearchCase::IgnoreCase)
			|| CleanFilename.StartsWith(TEXT(".uegit-restore-"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (bRepositoryMetadata)
		{
			PendingRepositoryMetadataInvalidations.Add(CanonicalRepositoryRoot);
			continue;
		}
		if (Filename == GitSourceControlProviderPrivate::NormalizeCacheFilename(FPaths::Combine(CanonicalRepositoryRoot, TEXT(".git")))
			|| FPaths::DirectoryExists(Filename) || FPaths::GetExtension(Filename).IsEmpty())
		{
			PendingRepositoryMetadataInvalidations.Add(CanonicalRepositoryRoot);
			continue;
		}

		FString EventRepositoryRoot = CanonicalRepositoryRoot;
		for (const FDirectoryWatch& Watch : DirectoryWatches)
		{
			if (Watch.RepositoryRoot.Len() > EventRepositoryRoot.Len() && FPaths::IsUnderDirectory(Filename, Watch.RepositoryRoot))
			{
				EventRepositoryRoot = Watch.RepositoryRoot;
			}
		}
		PendingChangedPathsByRepository.FindOrAdd(EventRepositoryRoot).Add(Filename);
	}

	for (TPair<FString, TSet<FString>>& Pair : PendingChangedPathsByRepository)
	{
		if (Pair.Value.Num() > 128)
		{
			PendingRepositoryMetadataInvalidations.Add(Pair.Key);
			Pair.Value.Empty();
		}
	}
	PendingWatchInvalidationTime = FPlatformTime::Seconds() + 0.25;
}

bool FGitSourceControlProvider::ApplyPendingDirectoryChanges()
{
	if (PendingWatchInvalidationTime == 0.0 || FPlatformTime::Seconds() < PendingWatchInvalidationTime)
	{
		return false;
	}

	TMap<FString, TSet<FString>> ChangedPathsByRepository = MoveTemp(PendingChangedPathsByRepository);
	TSet<FString> MetadataInvalidations = MoveTemp(PendingRepositoryMetadataInvalidations);
	PendingWatchInvalidationTime = 0.0;

	bool bInvalidatedCache = false;
	for (const FString& RepositoryRoot : MetadataInvalidations)
	{
		GitSourceControlUtils::InvalidateRepository(RepositoryRoot);
		bInvalidatedCache = true;
	}

	if (bInvalidatedCache)
	{
		FScopeLock Lock(&StateCacheCriticalSection);
		TArray<FString> CacheKeysToRemove;
		for (const TPair<FString, TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>>& Pair : StateCache)
		{
			const FString StateRepositoryRoot = GitSourceControlProviderPrivate::ResolveNearestRepositoryRoot(Pair.Key, PathToRepositoryRoot);
			if (MetadataInvalidations.Contains(StateRepositoryRoot))
			{
				CacheKeysToRemove.Add(Pair.Key);
			}
		}
		for (const FString& CacheKey : CacheKeysToRemove)
		{
			StateCache.Remove(CacheKey);
		}
	}

	for (const TPair<FString, TSet<FString>>& Pair : ChangedPathsByRepository)
	{
		if (MetadataInvalidations.Contains(Pair.Key) || Pair.Value.IsEmpty())
		{
			continue;
		}

		TMap<FString, TArray<FString>> FilesByRepository;
		for (const FString& Filename : Pair.Value)
		{
			const FString RepositoryRoot = GitSourceControlProviderPrivate::ResolveNearestRepositoryRoot(Filename, Pair.Key);
			if (!RepositoryRoot.IsEmpty())
			{
				FilesByRepository.FindOrAdd(RepositoryRoot).Add(Filename);
			}
		}

		for (const TPair<FString, TArray<FString>>& RepositoryFiles : FilesByRepository)
		{
			RegisterDirectoryWatchesForRoot(RepositoryFiles.Key, RepositoryFiles.Key == GitSourceControlProviderPrivate::NormalizeCacheFilename(PathToGitRoot));
			GitSourceControlUtils::InvalidateRepository(RepositoryFiles.Key);
			QueueStatusRefresh(RepositoryFiles.Value);
		}
	}
	return bInvalidatedCache;
}

void FGitSourceControlProvider::QueueStatusRefresh(const TArray<FString>& Filenames)
{
	for (const FString& Filename : Filenames)
	{
		const FString RepositoryRoot = ResolveRepositoryRootForFile(Filename);
		if (!RepositoryRoot.IsEmpty())
		{
			PendingStatusRefreshesByRepository.FindOrAdd(RepositoryRoot).Add(GitSourceControlProviderPrivate::NormalizeCacheFilename(Filename));
		}
	}
}

void FGitSourceControlProvider::IssuePendingStatusRefreshes()
{
	TMap<FString, TSet<FString>> PendingRefreshes = MoveTemp(PendingStatusRefreshesByRepository);
	for (const TPair<FString, TSet<FString>>& PendingRefresh : PendingRefreshes)
	{
		if (PendingRefresh.Value.IsEmpty())
		{
			continue;
		}

		TSet<FString> OutstandingPaths;
		bool bStatusRequestInFlight = false;
		for (const FGitSourceControlCommand* Command : CommandQueue)
		{
			if (Command->Operation->GetName() != TEXT("UpdateStatus") || Command->PathToRepositoryRoot != PendingRefresh.Key || Command->bExecuteProcessed)
			{
				continue;
			}

			bStatusRequestInFlight = true;
			for (const FString& Filename : PendingRefresh.Value)
			{
				if (!Command->Files.Contains(Filename))
				{
					OutstandingPaths.Add(Filename);
				}
			}
			break;
		}

		if (bStatusRequestInFlight)
		{
			for (const FString& Filename : OutstandingPaths)
			{
				PendingStatusRefreshesByRepository.FindOrAdd(PendingRefresh.Key).Add(Filename);
			}
			continue;
		}

		RegisterDirectoryWatchesForRoot(PendingRefresh.Key, PendingRefresh.Key == GitSourceControlProviderPrivate::NormalizeCacheFilename(PathToGitRoot));
		const ECommandResult::Type Result = Execute(
			ISourceControlOperation::Create<FUpdateStatus>(),
			FSourceControlChangelistPtr(),
			PendingRefresh.Value.Array(),
			EConcurrency::Asynchronous);
		if (Result != ECommandResult::Succeeded)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Failed to schedule local Git status refresh."));
		}
	}
}

void FGitSourceControlProvider::SetLastErrors(const TArray<FText>& InErrors)
{

	FScopeLock Lock(&LastErrorsCriticalSection);
	LastErrors = InErrors;
}

TArray<FText> FGitSourceControlProvider::GetLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	TArray<FText> Result = LastErrors;
	return Result;
}

int32 FGitSourceControlProvider::GetNumLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	return LastErrors.Num();
}

void FGitSourceControlProvider::Close()
{
	UnregisterDirectoryWatchers();

	TArray<FGitSourceControlCommand*> CommandsToJoin = MoveTemp(CommandQueue);
	for (FGitSourceControlCommand* Command : CommandsToJoin)
	{
		Command->Cancel();
		if (GThreadPool != nullptr && GThreadPool->RetractQueuedWork(Command))
		{
			Command->Abandon();
		}
	}

	for (FGitSourceControlCommand* Command : CommandsToJoin)
	{
		Command->WaitForCompletion();
		delete Command;
	}
	CommandGenerations.Empty();

	{
		FScopeLock Lock(&StateCacheCriticalSection);
		StateCache.Empty();
	}

	bGitAvailable = false;
	bGitRepositoryFound = false;
	PathToGitBinary.Reset();
	PathToGitRoot.Reset();
	PathToRepositoryRoot.Reset();
	GitVersion = FGitVersion();
	{
		FScopeLock Lock(&LastErrorsCriticalSection);
		LastErrors.Empty();
	}
}

TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> FGitSourceControlProvider::GetStateInternal(const FString& Filename)
{
	const FString CacheFilename = GitSourceControlProviderPrivate::NormalizeCacheFilename(Filename);
	const FString DisplayFilename = GitSourceControlProviderPrivate::NormalizeDisplayFilename(Filename);
	FScopeLock Lock(&StateCacheCriticalSection);
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(CacheFilename);
	if (State != NULL)
	{
		// Keep the cache key canonical while preserving the reliable path spelling
		// for UE's asset and History/Diff UI. Only the game thread mutates states.
		if (IsInGameThread() && !DisplayFilename.IsEmpty() && (*State)->LocalFilename != DisplayFilename)
		{
			(*State)->LocalFilename = DisplayFilename;
		}
		return (*State);
	}
	else
	{
		// cache an unknown state for this item
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> NewState = MakeShareable(new FGitSourceControlState(DisplayFilename));
		StateCache.Add(CacheFilename, NewState);
		return NewState;
	}
}

FText FGitSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("IsAvailable"), (IsEnabled() && IsAvailable()) ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add( TEXT("RepositoryName"), FText::FromString(PathToRepositoryRoot) );

	FText FormattedError;
	const TArray<FText>& RecentErrors = GetLastErrors();
	if (RecentErrors.Num() > 0)
	{
		FFormatNamedArguments ErrorArgs;
		ErrorArgs.Add(TEXT("ErrorText"), RecentErrors[0]);

		FormattedError = FText::Format(LOCTEXT("GitErrorStatusText", "Error: {ErrorText}\n\n"), ErrorArgs);
	}

	Args.Add(TEXT("ErrorText"), FormattedError);

	return FText::Format(NSLOCTEXT("GitStatusText", "{ErrorText}Enabled: {IsAvailable}", "Local repository: {RepositoryName}"), Args);
}

/** Quick check if revision control is enabled */
bool FGitSourceControlProvider::IsEnabled() const
{
	return bGitRepositoryFound;
}

/** Quick check if revision control is available for use (useful for server-based providers) */
bool FGitSourceControlProvider::IsAvailable() const
{
	return bGitRepositoryFound;
}

const FName& FGitSourceControlProvider::GetName(void) const
{
	return ProviderName;
}

FString FGitSourceControlProvider::ResolveRepositoryRootForFile(const FString& Filename) const
{
	if (PathToRepositoryRoot.IsEmpty())
	{
		return FString();
	}
	return GitSourceControlProviderPrivate::ResolveNearestRepositoryRoot(Filename, PathToRepositoryRoot);
}

ECommandResult::Type FGitSourceControlProvider::GetState( const TArray<FString>& InFiles, TArray< TSharedRef<ISourceControlState, ESPMode::ThreadSafe> >& OutState, EStateCacheUsage::Type InStateCacheUsage )
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		TArray<FString> ForceUpdate;
		ForceUpdate.Reserve(InFiles.Num());
		for (const FString& Filename : InFiles)
		{
			ForceUpdate.AddUnique(GitSourceControlProviderPrivate::NormalizeCacheFilename(Filename));
		}
		if (ForceUpdate.Num() > 0)
		{
			QueueStatusRefresh(ForceUpdate);
		}
	}

	for (const FString& Filename : InFiles)
	{
		OutState.Add(GetStateInternal(Filename));
	}

	return ECommandResult::Succeeded;
}

#if ENGINE_MAJOR_VERSION >= 5
ECommandResult::Type FGitSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	(void)InChangelists;
	(void)OutState;
	(void)InStateCacheUsage;
	return ECommandResult::Failed;
}
#endif

TArray<FSourceControlStateRef> FGitSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	FScopeLock Lock(&StateCacheCriticalSection);
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		const FSourceControlStateRef& State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

bool FGitSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	const FString CacheFilename = GitSourceControlProviderPrivate::NormalizeCacheFilename(Filename);
	FScopeLock Lock(&StateCacheCriticalSection);
	return StateCache.Remove(CacheFilename) > 0;
}

/** Get files in cache */
TArray<FString> FGitSourceControlProvider::GetFilesInCache()
{
	FScopeLock Lock(&StateCacheCriticalSection);
	TArray<FString> Files;
	for (const auto& State : StateCache)
	{
		Files.Add(State.Key);
	}
	return Files;
}

FDelegateHandle FGitSourceControlProvider::RegisterSourceControlStateChanged_Handle( const FSourceControlStateChanged::FDelegate& SourceControlStateChanged )
{
	return OnSourceControlStateChanged.Add( SourceControlStateChanged );
}

void FGitSourceControlProvider::UnregisterSourceControlStateChanged_Handle( FDelegateHandle Handle )
{
	OnSourceControlStateChanged.Remove( Handle );
}

#if ENGINE_MAJOR_VERSION < 5
ECommandResult::Type FGitSourceControlProvider::Execute( const FSourceControlOperationRef& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#else
ECommandResult::Type FGitSourceControlProvider::Execute( const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#endif
{
	if(!IsEnabled() && !(InOperation->GetName() == "Connect")) // Only Connect operation allowed while not Enabled (Repository found)
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);
	TMap<FString, TArray<FString>> FilesByRepository;
	for (const FString& Filename : AbsoluteFiles)
	{
		const FString RepositoryRoot = GitSourceControlProviderPrivate::ResolveNearestRepositoryRoot(Filename, PathToRepositoryRoot);
		if (RepositoryRoot.IsEmpty())
		{
			const FText Message = FText::Format(LOCTEXT("FileOutsideRepository", "File '{0}' is outside the connected Git repository."), FText::FromString(Filename));
			InOperation->AddErrorMessge(Message);
			InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
			return ECommandResult::Failed;
		}
		FilesByRepository.FindOrAdd(RepositoryRoot).Add(Filename);
	}

	// FSourceControlWindows requests History once through the provider and then
	// performs its own synchronous state update before opening the dialog. If an
	// explicit History prefetch already completed for the current repository
	// generation, satisfy that second request from the immutable cache instead of
	// starting a duplicate git log process (and blocking the editor again).
	if (InOperation->GetName() == TEXT("UpdateStatus") && InConcurrency == EConcurrency::Synchronous && FilesByRepository.Num() == 1)
	{
		const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOperation = StaticCastSharedRef<FUpdateStatus>(InOperation);
		if (StatusOperation->ShouldUpdateHistory())
		{
			const TPair<FString, TArray<FString>>& RepositoryFiles = *FilesByRepository.CreateConstIterator();
			const uint64 CurrentGeneration = GitSourceControlUtils::GetRepositoryGeneration(RepositoryFiles.Key);
			bool bHistoryIsFresh = !RepositoryFiles.Value.IsEmpty();
			for (const FString& Filename : RepositoryFiles.Value)
			{
				const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(Filename);
				if (State->History.IsEmpty() || State->HistoryGeneration != CurrentGeneration)
				{
					bHistoryIsFresh = false;
					break;
				}
			}
			if (bHistoryIsFresh)
			{
				InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Succeeded);
				return ECommandResult::Succeeded;
			}
		}
	}

	if (FilesByRepository.Num() > 1)
	{
		if (InOperation->GetName() == TEXT("UpdateStatus"))
		{
			const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> SourceOperation = StaticCastSharedRef<FUpdateStatus>(InOperation);
			if (InConcurrency == EConcurrency::Asynchronous)
			{
				const TSharedRef<GitSourceControlProviderPrivate::FUpdateStatusBatch, ESPMode::ThreadSafe> Batch = MakeShared<GitSourceControlProviderPrivate::FUpdateStatusBatch, ESPMode::ThreadSafe>(InOperation, InOperationCompleteDelegate, FilesByRepository.Num());
				for (const TPair<FString, TArray<FString>>& RepositoryFiles : FilesByRepository)
				{
					const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> RepositoryOperation = GitSourceControlProviderPrivate::CloneUpdateStatusOperation(SourceOperation.Get());
					Execute(RepositoryOperation, FSourceControlChangelistPtr(), RepositoryFiles.Value, EConcurrency::Asynchronous,
						FSourceControlOperationComplete::CreateLambda([Batch, RepositoryOperation](const FSourceControlOperationRef&, const ECommandResult::Type RepositoryResult)
						{
							Batch->Operation->AppendResultInfo(RepositoryOperation->GetResultInfo());
							if (Batch->Result == ECommandResult::Succeeded && RepositoryResult != ECommandResult::Succeeded)
							{
								Batch->Result = RepositoryResult;
							}
							if (--Batch->Remaining == 0)
							{
								Batch->CompletionDelegate.ExecuteIfBound(Batch->Operation, Batch->Result);
							}
						}));
				}
				return ECommandResult::Succeeded;
			}

			ECommandResult::Type Result = ECommandResult::Succeeded;
			for (const TPair<FString, TArray<FString>>& RepositoryFiles : FilesByRepository)
			{
				const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> RepositoryOperation = GitSourceControlProviderPrivate::CloneUpdateStatusOperation(SourceOperation.Get());

				Result = Execute(RepositoryOperation, FSourceControlChangelistPtr(), RepositoryFiles.Value, EConcurrency::Synchronous);
				InOperation->AppendResultInfo(RepositoryOperation->GetResultInfo());
				if (Result != ECommandResult::Succeeded)
				{
					break;
				}
			}
			InOperationCompleteDelegate.ExecuteIfBound(InOperation, Result);
			return Result;
		}

		const FText Message = LOCTEXT("MixedRepositoryOperation", "Selected files belong to multiple Git repositories. Refresh each repository separately.");
		FTSMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	// Query to see if we allow this operation
	TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if(!Worker.IsValid())
	{
		// this operation is unsupported by this revision control provider
		FFormatNamedArguments Arguments;
		Arguments.Add( TEXT("OperationName"), FText::FromName(InOperation->GetName()) );
		Arguments.Add( TEXT("ProviderName"), FText::FromName(GetName()) );
		FText Message(FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments));

		FTSMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);

		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FGitSourceControlCommand* Command = new FGitSourceControlCommand(InOperation, Worker.ToSharedRef());
	if (FilesByRepository.Num() == 1)
	{
		const TPair<FString, TArray<FString>>& RepositoryFiles = *FilesByRepository.CreateConstIterator();
		Command->PathToRepositoryRoot = RepositoryFiles.Key;
		Command->PathToGitRoot = RepositoryFiles.Key;
		AbsoluteFiles = RepositoryFiles.Value;
		RegisterDirectoryWatchesForRoot(RepositoryFiles.Key, RepositoryFiles.Key == GitSourceControlProviderPrivate::NormalizeCacheFilename(PathToGitRoot));
	}
	Command->Files = AbsoluteFiles;
	Command->OperationCompleteDelegate = InOperationCompleteDelegate;
	CommandGenerations.Add(Command, GitSourceControlUtils::GetRepositoryGeneration(Command->PathToRepositoryRoot));

#if ENGINE_MAJOR_VERSION == 5
	(void)InChangelist;
#endif
	
	// fire off operation
	if(InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("ExecuteSynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString(), false);
	}
	else
	{
		Command->bAutoDelete = true;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("IssueAsynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return IssueCommand(*Command);
	}
}

#if ENGINE_MAJOR_VERSION < 5
bool FGitSourceControlProvider::CanCancelOperation( const FSourceControlOperationRef& InOperation ) const
#else
bool FGitSourceControlProvider::CanCancelOperation( const FSourceControlOperationRef& InOperation ) const
#endif
{
	const FName OperationName = InOperation->GetName();
	if (OperationName != TEXT("Connect") && OperationName != TEXT("UpdateStatus"))
	{
		return false;
	}

	for (const FGitSourceControlCommand* Command : CommandQueue)
	{
		if (Command->Operation == InOperation && !Command->bExecuteProcessed)
		{
			return true;
		}
	}
	return false;
}

#if ENGINE_MAJOR_VERSION < 5
void FGitSourceControlProvider::CancelOperation( const FSourceControlOperationRef& InOperation )
#else
void FGitSourceControlProvider::CancelOperation( const FSourceControlOperationRef& InOperation )
#endif
{
	const FName OperationName = InOperation->GetName();
	if (OperationName != TEXT("Connect") && OperationName != TEXT("UpdateStatus"))
	{
		return;
	}

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			Command.Cancel();
			return;
		}
	}
}

bool FGitSourceControlProvider::UsesLocalReadOnlyState() const
{
	return false;
}

bool FGitSourceControlProvider::UsesChangelists() const
{
	return false;
}

bool FGitSourceControlProvider::UsesCheckout() const
{
	return false;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
bool FGitSourceControlProvider::UsesFileRevisions() const
{
	return true;
}

TOptional<bool> FGitSourceControlProvider::IsAtLatestRevision() const
{
	return TOptional<bool>();
}

TOptional<int> FGitSourceControlProvider::GetNumLocalChanges() const
{
	return TOptional<int>();
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
bool FGitSourceControlProvider::AllowsDiffAgainstDepot() const
{
	return true;
}

bool FGitSourceControlProvider::UsesUncontrolledChangelists() const
{
	return false;
}

bool FGitSourceControlProvider::UsesSnapshots() const
{
	return false;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
bool FGitSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const {
	return WorkersMap.Find(InOperation->GetName()) != nullptr && (IsEnabled() || InOperation->GetName() == TEXT("Connect"));
}

TMap<ISourceControlProvider::EStatus, FString> FGitSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No") );
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No") );
	Result.Add(EStatus::Repository, PathToRepositoryRoot);
	return Result;
}
#endif

TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> FGitSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	const FGetGitSourceControlWorker* Operation = WorkersMap.Find(InOperationName);
	if(Operation != nullptr)
	{
		return Operation->Execute();
	}

	return nullptr;
}

void FGitSourceControlProvider::RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate )
{
	WorkersMap.Add( InName, InDelegate );
}

void FGitSourceControlProvider::OutputCommandMessages(const FGitSourceControlCommand& InCommand) const
{
	FTSMessageLog SourceControlLog("SourceControl");

	for (int32 ErrorIndex = 0; ErrorIndex < InCommand.ResultInfo.ErrorMessages.Num(); ++ErrorIndex)
	{
		SourceControlLog.Error(FText::FromString(InCommand.ResultInfo.ErrorMessages[ErrorIndex]));
	}

	for (int32 InfoIndex = 0; InfoIndex < InCommand.ResultInfo.InfoMessages.Num(); ++InfoIndex)
	{
		SourceControlLog.Info(FText::FromString(InCommand.ResultInfo.InfoMessages[InfoIndex]));
	}
}

void FGitSourceControlProvider::Tick()
{
	bool bStatesUpdated = ApplyPendingDirectoryChanges();
	IssuePendingStatusRefreshes();

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];

		if (Command.bExecuteProcessed)
		{
			const uint64 CommandGeneration = CommandGenerations.FindRef(&Command);
			const bool bCurrentGeneration = CommandGeneration == GitSourceControlUtils::GetRepositoryGeneration(Command.PathToRepositoryRoot);
			CommandGenerations.Remove(&Command);
			if (!bCurrentGeneration)
			{
				Command.Cancel();
			}

			// Remove command from the queue
			CommandQueue.RemoveAt(CommandIndex);

			if (!Command.IsCanceled() && bCurrentGeneration)
			{
				// Let the completed command update only the states it queried.
				bStatesUpdated |= Command.Worker->UpdateStates();
			}

			// dump any messages to output log
			OutputCommandMessages(Command);

			// 先从队列移除再回调, 保证正常完成和取消都只通知一次.
			Command.ReturnResults();

			// commands that are left in the array during a tick need to be deleted
			if(Command.bAutoDelete)
			{
				// Only delete commands that are not running 'synchronously'
				delete &Command;
			}

			// only do one command per tick loop, as we dont want concurrent modification
			// of the command queue (which can happen in the completion delegate)
			break;
		}
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TArray< TSharedRef<ISourceControlLabel> > FGitSourceControlProvider::GetLabels( const FString& InMatchingSpec ) const
{
	(void)InMatchingSpec;
	TArray< TSharedRef<ISourceControlLabel> > Tags;

	// NOTE list labels. Called by CrashDebugHelper() (to remote debug Engine crash)
	//					 and by SourceControlHelpers::AnnotateFile() (to add source file to report)
	// Reserved for internal use by Epic Games with Perforce only
	return Tags;
}

#if ENGINE_MAJOR_VERSION >= 5
TArray<FSourceControlChangelistRef> FGitSourceControlProvider::GetChangelists( EStateCacheUsage::Type InStateCacheUsage )
{
	(void)InStateCacheUsage;
	return TArray<FSourceControlChangelistRef>();
}
#endif

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FGitSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SGitSourceControlSettings);
}
#endif

ECommandResult::Type FGitSourceControlProvider::ExecuteSynchronousCommand(FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	struct Local
	{
		static void CancelCommand(FGitSourceControlCommand* InControlCommand)
		{
			InControlCommand->Cancel();
		}
	};

	FText TaskText = Task;
	// Display the progress dialog
	if (bSuppressResponseMsg)
	{
		TaskText = FText::GetEmpty();
	}

	int i = 0;

	// Display the progress dialog if a string was provided
	{
		// TODO: support cancellation?
		//FScopedSourceControlProgress Progress(TaskText, FSimpleDelegate::CreateStatic(&Local::CancelCommand, &InCommand));
		FScopedSourceControlProgress Progress(TaskText);

		// Issue the command asynchronously...
		IssueCommand( InCommand );

		// ... then wait for its completion (thus making it synchronous)
		while (!InCommand.IsCanceled() && CommandQueue.Contains(&InCommand))
		{
			// Tick the command queue and update progress.
			Tick();

			if (i >= 20) {
				Progress.Tick();
				i = 0;
			}
			i++;

			// Sleep for a bit so we don't busy-wait so much.
			FPlatformProcess::Sleep(0.01f);
		}

		if (InCommand.bCancelled)
		{
			Result = ECommandResult::Cancelled;
		}
		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
		else if (!bSuppressResponseMsg)
		{
			FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("Git_ServerUnresponsive", "Git command failed. Please check your connection and try again, or check the output log for more information.") );
			UE_LOG(LogSourceControl, Error, TEXT("Command '%s' Failed!"), *InCommand.Operation->GetName().ToString());
		}
	}

	// Delete the command now if not marked as auto-delete
	if (!InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

ECommandResult::Type FGitSourceControlProvider::IssueCommand(FGitSourceControlCommand& InCommand, const bool bSynchronous)
{
	if (!bSynchronous && GThreadPool != nullptr)
	{
		// Queue this to our worker thread(s) for resolving.
		// When asynchronous, any callback gets called from Tick().
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}
	else
	{
		UE_LOG(LogSourceControl, Log, TEXT("There are no threads available to process the revision control command '%s'. Running synchronously."), *InCommand.Operation->GetName().ToString());

		InCommand.bCommandSuccessful = InCommand.DoWork();

		const uint64 CommandGeneration = CommandGenerations.FindRef(&InCommand);
		const bool bCurrentGeneration = CommandGeneration == GitSourceControlUtils::GetRepositoryGeneration(InCommand.PathToRepositoryRoot);
		CommandGenerations.Remove(&InCommand);
		if (bCurrentGeneration)
		{
			InCommand.Worker->UpdateStates();
		}
		else
		{
			InCommand.Cancel();
		}

		OutputCommandMessages(InCommand);

		// Callback now if present. When asynchronous, this callback gets called from Tick().
		return InCommand.ReturnResults();
	}
}

bool FGitSourceControlProvider::QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest)
{
	(void)ConfigSrc;
	(void)ConfigDest;
	return false;
}

void FGitSourceControlProvider::RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn)
{
	(void)BranchNames;
	(void)ContentRootIn;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
bool FGitSourceControlProvider::GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const
{
	(void)BranchIndex;
	(void)OutBranchName;
	return false;
}
#endif

int32 FGitSourceControlProvider::GetStateBranchIndex(const FString& StateBranchName) const
{
	(void)StateBranchName;
	return INDEX_NONE;
}

#undef LOCTEXT_NAMESPACE
