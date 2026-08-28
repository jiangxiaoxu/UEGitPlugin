// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "GitSourceControlHistoryMode.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/Object.h"

#include "GitLocalSourceControl.generated.h"

/** 生命周期中的 Git Local SourceControl 异步操作阶段. */
UENUM()
enum class EGitLocalSourceControlOperationPhase : uint8
{
	Queued,
	LoadingHistory,
	FetchingLfs,
	Preparing,
	Mutating,
	Reloading,
	Completed,
	Cancelled,
	Failed,
};

/** 当前 Editor 可用的 standalone local Git repository snapshot. */
USTRUCT(meta = (ForceAngelscriptBind))
struct GITSOURCECONTROL_API FGitLocalSourceControlProviderInfo
{
	GENERATED_BODY()

	UPROPERTY(ScriptReadOnly)
	bool bAvailable = false;

	UPROPERTY(ScriptReadOnly)
	FString GitBinary;

	UPROPERTY(ScriptReadOnly)
	FString RepositoryRoot;

	UPROPERTY(ScriptReadOnly)
	FString Error;
};

/** 一条机器可读的单文件 Git revision. */
USTRUCT(meta = (ForceAngelscriptBind))
struct GITSOURCECONTROL_API FGitLocalSourceControlHistoryEntry
{
	GENERATED_BODY()

	UPROPERTY(ScriptReadOnly)
	FString CommitId;

	UPROPERTY(ScriptReadOnly)
	FString HistoricalPath;

	UPROPERTY(ScriptReadOnly)
	FString Description;

	UPROPERTY(ScriptReadOnly)
	FString Author;

	UPROPERTY(ScriptReadOnly)
	FString Action;

	UPROPERTY(ScriptReadOnly)
	FString DateUtc;
};

/** 一次异步本地 Git 操作的最终结果. */
USTRUCT(meta = (ForceAngelscriptBind))
struct GITSOURCECONTROL_API FGitLocalSourceControlOperationResult
{
	GENERATED_BODY()

	UPROPERTY(ScriptReadOnly)
	bool bSucceeded = false;

	UPROPERTY(ScriptReadOnly)
	bool bCancelled = false;

	UPROPERTY(ScriptReadOnly)
	bool bReloadSucceeded = true;

	UPROPERTY(ScriptReadOnly)
	TArray<FString> AffectedFiles;

	UPROPERTY(ScriptReadOnly)
	TArray<FString> Errors;

	UPROPERTY(ScriptReadOnly)
	TArray<FGitLocalSourceControlHistoryEntry> History;
};

struct FGitLocalSourceControlOperationState;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGitLocalSourceControlOperationProgressEvent, EGitLocalSourceControlOperationPhase, Phase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGitLocalSourceControlOperationCompletedEvent, FGitLocalSourceControlOperationResult, Result);

namespace GitLocalSourceControlPrivate
{
	class FOperationManager;
}

/**
 * Game-thread owned handle for one local Git operation. The module drives it until terminal.
 * Cancel only succeeds while the operation remains in a read-only or LFS-fetch phase.
 */
UCLASS(meta = (ScriptName = "GitLocalSourceControlOperation"))
class GITSOURCECONTROL_API UGitLocalSourceControlOperation final : public UObject
{
	GENERATED_BODY()

public:
	/** Request cancellation of a read-only/history/LFS phase. */
	UFUNCTION(ScriptCallable)
	bool Cancel();

	UFUNCTION(ScriptCallable)
	bool IsTerminal() const;

	UFUNCTION(ScriptCallable)
	EGitLocalSourceControlOperationPhase GetPhase() const;

	UFUNCTION(ScriptCallable)
	FGitLocalSourceControlOperationResult GetResult() const;

	UPROPERTY(ScriptReadWrite)
	FGitLocalSourceControlOperationProgressEvent OnProgress;

	UPROPERTY(ScriptReadWrite)
	FGitLocalSourceControlOperationCompletedEvent OnCompleted;

	/** Arms one tracked-file discard for a mutation operation; read-only operations reject the request. */
	UFUNCTION(ScriptCallable)
	bool ScheduleDiscardTrackedAfterCompletion(const TArray<FString>& AssetObjectPaths);

	/** Internal C++ initialization. New operations are created only by the library. */
	void Initialize(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState);

private:
	friend class GitLocalSourceControlPrivate::FOperationManager;

	void PumpOnGameThread(bool bBroadcastNotifications, bool bAllowShutdownCleanup);
	TArray<FString> DeferredDiscardObjectPaths;
	TSharedPtr<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State;
	FGitLocalSourceControlOperationResult Result;
	EGitLocalSourceControlOperationPhase Phase = EGitLocalSourceControlOperationPhase::Queued;
	bool bTerminal = false;
};

/**
 * Editor-only typed local Git operations for AngelScript automation.
 * Inputs are asset object paths, never shell commands. Remote access is limited to an
 * exact historical LFS revision fetch requested by the caller.
 */
UCLASS(meta = (ScriptName = "GitLocalSourceControl"))
class GITSOURCECONTROL_API UGitLocalSourceControlLibrary final : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable)
	static FGitLocalSourceControlProviderInfo GetProviderInfo(const FString& AssetObjectPath);

	UFUNCTION(ScriptCallable)
	/** 加载单个资产的当前路径历史或已提交 R100 rename 链. */
	static UGitLocalSourceControlOperation* StartLoadHistory(const FString& AssetObjectPath, EGitLocalSourceControlHistoryMode Mode);

	UFUNCTION(ScriptCallable)
	static UGitLocalSourceControlOperation* StartFetchLfsRevision(const FString& AssetObjectPath, const FString& Revision);

/** Force-restores one same-path historical revision. Working, staged, conflicted, untracked, and in-memory changes are discarded without undo. */
	UFUNCTION(ScriptCallable)
	static UGitLocalSourceControlOperation* StartRestoreRevision(const FString& AssetObjectPath, const FString& Revision);

	/** Discards index and worktree changes only for clean tracked .uasset files and schedules reload after completion. */
	UFUNCTION(ScriptCallable)
	static UGitLocalSourceControlOperation* StartDiscardTracked(const TArray<FString>& AssetObjectPaths);
};

/** Cancels and joins API workers before the owning Editor module unloads. */
namespace GitLocalSourceControl
{
	/** Starts the module-owned Game Thread operation pump. Called during module startup. */
	GITSOURCECONTROL_API void StartupOperations();

	GITSOURCECONTROL_API void ShutdownOperations();

#if WITH_DEV_AUTOMATION_TESTS
	namespace Testing
	{
		GITSOURCECONTROL_API void PumpOperations();
		GITSOURCECONTROL_API int32 GetManagedOperationCount();
		GITSOURCECONTROL_API bool HasOperationTicker();
		GITSOURCECONTROL_API void SetForcePreparedPackageReloadFailure(bool bEnabled);
		GITSOURCECONTROL_API void SetForceRestoreWorktreeRollback(bool bEnabled);
		GITSOURCECONTROL_API FString GetLastDeferredCleanupDiagnostic();
		GITSOURCECONTROL_API void ClearLastDeferredCleanupDiagnostic();
		GITSOURCECONTROL_API void ResetDeferredCleanupLaunchCount();
		GITSOURCECONTROL_API int32 GetDeferredCleanupLaunchCount();
		GITSOURCECONTROL_API UGitLocalSourceControlOperation* StartBlockedReadOnlyOperationForTesting();
		GITSOURCECONTROL_API bool WaitForBlockedReadOnlyOperationToReachFinalPublication(double TimeoutSeconds = 15.0);
		GITSOURCECONTROL_API void ReleaseBlockedReadOnlyOperation();
		GITSOURCECONTROL_API bool DrainOperationsForTesting();
	}
#endif
}
