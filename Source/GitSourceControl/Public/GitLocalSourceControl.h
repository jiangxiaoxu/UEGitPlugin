// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/Object.h"

#include "GitLocalSourceControl.generated.h"

/** 生命周期中的 Git Local SourceControl 异步操作阶段. */
UENUM(BlueprintType)
enum class EGitLocalSourceControlOperationPhase : uint8
{
	Queued,
	LoadingHistory,
	FetchingLfs,
	Preparing,
	Mutating,
	Refreshing,
	Completed,
	Cancelled,
	Failed,
};

/** 当前 Editor 可用的本地 Git provider 快照. */
USTRUCT(BlueprintType)
struct GITSOURCECONTROL_API FGitLocalSourceControlProviderInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	bool bAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString GitBinary;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString RepositoryRoot;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString Error;
};

/** 一条机器可读的单文件 Git revision. */
USTRUCT(BlueprintType)
struct GITSOURCECONTROL_API FGitLocalSourceControlHistoryEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString CommitId;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString HistoricalPath;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString Author;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString Action;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	FString DateUtc;
};

/** 一次异步本地 Git 操作的最终结果. */
USTRUCT(BlueprintType)
struct GITSOURCECONTROL_API FGitLocalSourceControlOperationResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	bool bCancelled = false;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	bool bReloadSucceeded = true;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	TArray<FString> AffectedFiles;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	TArray<FString> Errors;

	UPROPERTY(BlueprintReadOnly, Category = "Git Local SourceControl")
	TArray<FGitLocalSourceControlHistoryEntry> History;
};

struct FGitLocalSourceControlOperationState;

/**
 * Game-thread owned handle for one local Git operation. Call Tick until terminal.
 * Cancel only succeeds while the operation remains in a read-only or LFS-fetch phase.
 */
UCLASS(BlueprintType, meta = (ScriptName = "GitLocalSourceControlOperation"))
class GITSOURCECONTROL_API UGitLocalSourceControlOperation final : public UObject
{
	GENERATED_BODY()

public:
	/** Consumes completed worker output on the Game Thread. */
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	void Tick();

	/** Request cancellation of a read-only/history/LFS phase. */
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	bool Cancel();

	UFUNCTION(BlueprintPure, ScriptCallable, Category = "Git Local SourceControl")
	bool IsTerminal() const;

	UFUNCTION(BlueprintPure, ScriptCallable, Category = "Git Local SourceControl")
	EGitLocalSourceControlOperationPhase GetPhase() const;

	UFUNCTION(BlueprintPure, ScriptCallable, Category = "Git Local SourceControl")
	FGitLocalSourceControlOperationResult GetResult() const;

	/** Internal C++ initialization. New operations are created only by the library. */
	void Initialize(TSharedRef<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> InState);

private:
	TSharedPtr<FGitLocalSourceControlOperationState, ESPMode::ThreadSafe> State;
	FGitLocalSourceControlOperationResult Result;
	EGitLocalSourceControlOperationPhase CompletedPhase = EGitLocalSourceControlOperationPhase::Queued;
	bool bTerminal = false;
};

/**
 * Editor-only typed local Git operations for AngelScript and Blueprint automation.
 * Inputs are asset object paths, never shell commands. Remote access is limited to an
 * exact historical LFS revision fetch requested by the caller.
 */
UCLASS(meta = (ScriptName = "GitLocalSourceControl"))
class GITSOURCECONTROL_API UGitLocalSourceControlLibrary final : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static FGitLocalSourceControlProviderInfo GetProviderInfo();

	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartLoadHistory(const FString& AssetObjectPath);

	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartFetchLfsRevision(const FString& AssetObjectPath, const FString& Revision);

	/** Restores one historical revision to a clean, closed non-map asset workspace file. */
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartRestoreRevision(const FString& AssetObjectPath, const FString& Revision);

	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartRefreshStatus(const TArray<FString>& AssetObjectPaths);

	/** Discards index and worktree changes only for clean, closed selected asset files. */
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartDiscardTracked(const TArray<FString>& AssetObjectPaths);

	/** Deletes only clean, closed selected untracked asset files. */
	UFUNCTION(BlueprintCallable, ScriptCallable, Category = "Git Local SourceControl")
	static UGitLocalSourceControlOperation* StartDeleteUntracked(const TArray<FString>& AssetObjectPaths);
};

/** Cancels and joins API workers before the owning Editor module unloads. */
namespace GitLocalSourceControl
{
	GITSOURCECONTROL_API void ShutdownOperations();
}
