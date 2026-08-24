// Copyright (c) 2026
//
// Distributed under the MIT License (MIT).

#pragma once

#include "CoreMinimal.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"

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
		/** 在只读预检完成后调用, 只能收集用户确认. */
		TFunction<bool(const FString& OperationDescription, const TArray<FString>& AffectedFiles)> Confirm;

		/** 在确认和目标复核完成后、真正变更前调用. */
		TFunction<bool(const TArray<FString>& AffectedFiles)> PrepareForMutation;

		/** 仅在 Git/文件系统变更成功后调用. 返回 false 表示 package reload 失败. */
		TFunction<bool(const TArray<FString>& AffectedFiles)> ReloadPackages;
	};

	struct FGitAssetOperationResult
	{
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
		bool DeleteUntrackedFiles(const TArray<FString>& InFiles, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;
		bool RestoreRevisionToWorkspace(const FString& InCurrentFilename, const FString& InCommitId, const FString& InHistoricalPath,
			const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;

		static FGitAssetFileFingerprint CaptureFingerprint(const FString& InFilename);

		/** Resolve one nearest Git root; reject mixed-root requests instead of mutating a parent repository. */
		static bool ResolveSingleRepositoryRoot(const TArray<FString>& InFiles, const FString& InFallbackRepositoryRoot, FString& OutRepositoryRoot, FString& OutError);

	private:
		bool NormalizeFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles, FGitAssetOperationResult& OutResult) const;
		bool QueryStates(const TArray<FString>& InFiles, TMap<FString, FGitSourceControlState>& OutStates, FGitAssetOperationResult& OutResult) const;
		bool Confirm(const FString& Description, const TArray<FString>& Files, const FGitAssetOperationCallbacks& Callbacks, FGitAssetOperationResult& OutResult) const;
		bool RecheckTargets(const TArray<FString>& Files, const TMap<FString, FGitAssetFileFingerprint>& Fingerprints,
			const FGitIndexSnapshot& IndexSnapshot, uint64 Generation, const FString& ExpectedHeadCommitId, FGitAssetOperationResult& OutResult) const;

		FString GitBinary;
		FString RepositoryRoot;
	};
}
