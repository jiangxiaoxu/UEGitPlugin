// Copyright (c) 2026
//
// Distributed under the MIT License (MIT).

#pragma once

#include "CoreMinimal.h"
#include "GitSourceControlFileStatus.h"
#include "GitSourceControlUtils.h"

class UPackage;

/** UI 无关的本地 Git 资产变更服务. */
namespace GitSourceControlAssetOperations
{
	struct FGitAssetFileFingerprint
	{
		bool bExists = false;
		int64 Size = 0;
		FDateTime ModifiedTime;
		FString ContentHash;
		bool bHashValid = true;

		bool operator==(const FGitAssetFileFingerprint& Other) const
		{
			return bExists == Other.bExists && (!bExists ||
				(bHashValid && Other.bHashValid && Size == Other.Size && ModifiedTime == Other.ModifiedTime && ContentHash == Other.ContentHash));
		}
	};

	struct FGitAssetOperationCallbacks
	{
		/** 在等待 repository mutation guard 时查询取消状态. */
		TFunction<bool()> IsCancellationRequested;

		/** 在只读预检完成后调用, 只能收集用户确认. */
		TFunction<bool(const FString& OperationDescription, const TArray<FString>& AffectedFiles)> Confirm;

		/** 在确认和目标复核完成后, 真正变更前调用. 返回 false 后仍可能需要 ReloadPackages 恢复已卸载 package. */
		TFunction<bool(const TArray<FString>& AffectedFiles)> PrepareForMutation;

		/** 在 PrepareForMutation 后的每个终止路径调用, 用于恢复已卸载 package. 返回 false 表示 package reload 失败. */
		TFunction<bool(const TArray<FString>& AffectedFiles)> ReloadPackages;

#if WITH_DEV_AUTOMATION_TESTS
		/** Test seam executed after PrepareForMutation and immediately before the commit-point topology recheck. */
		TFunction<void()> BeforeCommitPointForTesting;

		/** Test seam executed after the exact-path index reset and before worktree replacement. */
		TFunction<bool()> AllowWorktreeReplaceForTesting;

		/** Test seam executed immediately before the exact-path Git mutation. Returning false simulates a Git mutation failure. */
		TFunction<bool(const FString& GitSubcommand)> AllowGitMutationForTesting;
#endif
	};

	struct FGitAssetOperationResult
	{
		/** True only when the disk mutation and required package reload both succeed. */
		bool bSucceeded = false;
		bool bCancelled = false;
		bool bReloadSucceeded = true;
		TArray<FString> AffectedFiles;
		TArray<FString> FailedFiles;
		TArray<FString> Errors;

		void AddError(const FString& Error)
		{
			if (!Error.IsEmpty())
			{
				Errors.Add(Error);
			}
		}
	};

	/**
	 * 仅对明确文件执行本地变更. 仅历史 Restore 在缺失 LFS object 时可执行精准远端下载;
	 * 服务不创建 commit, 也不负责 package/UI 生命周期.
	 */
	class GITSOURCECONTROL_API FGitSourceControlAssetOperations final
	{
	public:
		FGitSourceControlAssetOperations(FString InGitBinary, FString InRepositoryRoot);

		bool DiscardTrackedFiles(const TArray<FString>& InFiles, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;
		/** Force-restores one same-path revision: reset the exact index path to HEAD, replace the worktree blob, and roll both back on failure. */
		bool RestoreRevisionToWorkspace(const FString& InCurrentFilename, const FString& InCommitId, const FString& InHistoricalPath,
			const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;

		static FGitAssetFileFingerprint CaptureFingerprint(const FString& InFilename);

		/** Resolve one nearest Git root; reject mixed-root requests instead of mutating a parent repository. */
		static bool ResolveSingleRepositoryRoot(const TArray<FString>& InFiles, const FString& InFallbackRepositoryRoot, FString& OutRepositoryRoot, FString& OutError);

		/** Reject world and external packages; map mutations must use the logical map package operation service. */
		static bool ValidateStandaloneMutationPreflight(const TArray<FString>& InFiles, const TArray<UPackage*>& InLoadedPackages, FString& OutError);

	private:
		bool NormalizeFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles, FGitAssetOperationResult& OutResult) const;
		bool QueryStates(const TArray<FString>& InFiles, TMap<FString, FGitSourceControlFileStatus>& OutStates, FGitAssetOperationResult& OutResult) const;
		bool Confirm(const FString& Description, const TArray<FString>& Files, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;
		bool RecheckTargets(const TArray<FString>& Files, const TMap<FString, FGitAssetFileFingerprint>& Fingerprints,
			const FGitIndexSnapshot& IndexSnapshot, const FString& ExpectedHeadCommitId, FGitAssetOperationResult& OutResult) const;

		FString GitBinary;
		FString RepositoryRoot;
	};
}
