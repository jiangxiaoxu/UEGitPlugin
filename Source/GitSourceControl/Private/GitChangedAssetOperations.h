// Copyright (c) 2026
//
// Changed Assets 的 providerless Revert to HEAD 事务. 只处理一个 .uasset.

#pragma once

#include "CoreMinimal.h"
#include "GitChangedAssetsModel.h"

class UPackage;
class UWorld;

	namespace GitChangedAssetOperations
	{
	/** Revert 的私有诊断数据. 不属于 UI 或脚本结果契约. */
	struct FGitChangedAssetRevertTelemetry
	{
		int32 SelectedEntryCount = 0;
		int32 PlannedFileCount = 0;
		int32 UniqueLfsObjectCount = 0;
		int32 LfsPointerReadCount = 0;
		int32 LfsPointerCount = 0;
		int32 LfsVerifyCount = 0;
		int32 LfsFetchCount = 0;
		int32 GitHeadCheckCount = 0;
		int32 GitStatusCheckCount = 0;
		int32 GitIndexSnapshotCount = 0;
		int32 GitBlobReadCount = 0;
		int32 GitRestoreBatchCount = 0;
		int32 GitResetBatchCount = 0;
		int32 GitIndexRollbackCount = 0;
		int64 FingerprintBytes = 0;
		int64 BackupBytes = 0;
		double PreflightSeconds = 0.0;
		double ConfirmationSeconds = 0.0;
		double LfsSeconds = 0.0;
		double PrepareSeconds = 0.0;
		double LoaderResetSeconds = 0.0;
		double DiskMutationSeconds = 0.0;
		double EditorFinalizeSeconds = 0.0;
		double AssetRegistrySeconds = 0.0;
		double BrowserRefreshSeconds = 0.0;
		double PostRefreshStartSeconds = 0.0;
		double PostRefreshStatusSeconds = 0.0;
		double PostRefreshMetadataSeconds = 0.0;
		double PostRefreshTotalSeconds = 0.0;
		bool bDiskMutationSucceeded = false;
		bool bEditorReloadSucceeded = true;
		bool bCancelled = false;
		bool bPostRefreshAttempted = false;
		bool bPostRefreshSucceeded = false;
	};

	enum class EGitChangedAssetMutationOutcome : uint8
	{
		/** No selected .uasset or index entry was modified. */
		NeverMutated,
		/** A mutation was attempted and all selected disk/index state was restored. */
		RolledBack,
		/** A mutation was attempted but file or index recovery failed; preserve recovery backups. */
		RollbackFailed,
		/** The exact selected disk/index mutation completed successfully. */
		Succeeded,
	};

	struct FGitChangedAssetRevertResult
	{
		bool bSucceeded = false;
		bool bCancelled = false;
		bool bReloadSucceeded = true;
		TArray<FString> AffectedFiles;
		TArray<FString> FailedFiles;
		TArray<FString> Errors;

		void AddError(const FString& InError)
		{
			if (!InError.IsEmpty())
			{
				Errors.Add(InError);
			}
		}
	};

	/** 供确认 UI 展示的 GameThread lifecycle closure. */
	struct FGitChangedAssetRevertPreview
	{
		TArray<FString> SelectedDirtyPackageNames;
		TArray<FString> OwnerMapsToReload;
		FString ClosureSignature;
	};

	/**
	 * 所有回调都由调用方同步 marshaling 到 GameThread. Prepare 只能检查
	 * lifecycle closure; BeginMutation 仅在最后一次 recheck 和 backup 后执行
	 * loader reset. Finalize 始终获知精确 transaction outcome.
	 */
	struct FGitChangedAssetRevertCallbacks
	{
		/** Controller-owned private trace context. GameThread callbacks are synchronous with the worker. */
		TSharedPtr<FGitChangedAssetRevertTelemetry, ESPMode::ThreadSafe> Telemetry;

		/** 在等待 shared repository transaction guard 时响应 shutdown cancellation. */
		TFunction<bool()> IsCancellationRequested;

		/** 在 worker 已捕获 Git/index/worktree baseline 后, 同步到 GameThread 请求一次确认. */
		TFunction<bool(const TArray<FGitChangedAssetEntry>& Entries, FString& OutError)> Confirm;
		TFunction<bool(const TArray<FGitChangedAssetEntry>& Entries, FString& OutError)> PrepareForMutation;
		TFunction<bool(const TArray<FGitChangedAssetEntry>& Entries, FString& OutError)> BeginMutation;
		TFunction<bool(const TArray<FGitChangedAssetEntry>& Entries, const TArray<FString>& AffectedFiles, EGitChangedAssetMutationOutcome Outcome, FString& OutError)> FinalizeEditor;

#if WITH_DEV_AUTOMATION_TESTS
		/** 在 GameThread Prepare 后、commit-point fingerprint/index 重查前执行. */
		TFunction<void()> BeforeCommitPointForTesting;
		/** 在 Git index 已变更, 但本地 .uasset 删除前执行. */
		TFunction<bool()> AllowFilesystemMutationForTesting;
#endif
	};

	/**
	 * providerless Editor lifecycle. 仅能在 GameThread 使用;
	 * Prepare 在用户确认后只检查 closure; BeginMutation 在不可取消 commit point
	 * reset loader; Finish 在同一 worker 的 FinalizeEditor callback 内调用.
	 */
	class GITSOURCECONTROL_API FGitChangedAssetRevertLifecycle final
	{
	public:
		/** 必须在 GameThread 调用. 未选 dirty OFPA closure 会返回 false. */
		static bool BuildPreview(const TArray<FGitChangedAssetEntry>& InEntries, FGitChangedAssetRevertPreview& OutPreview, FString& OutError);
		/** 在确认对话框同一 GameThread 调用; Prepare/BeginMutation 必须保持相同 closure. */
		bool RecordConfirmedClosure(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError);
		bool Prepare(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError);
		bool BeginMutation(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError);
		bool Finish(const TArray<FGitChangedAssetEntry>& InEntries, const TArray<FString>& InAffectedFiles, EGitChangedAssetMutationOutcome InOutcome, FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
		/** Test-only override for constructing an isolated Editor-world lifecycle fixture. */
		static void SetCurrentEditorWorldForTesting(UWorld* InWorld);
		/** Test-only replacement for observing the final package reload batch without invoking PackageTools. */
		using FReloadPackagesForTesting = TFunction<bool(const TArray<UPackage*>& InPackages, FString& OutError)>;
		static void SetReloadPackagesForTesting(FReloadPackagesForTesting InReloadPackages);
#endif

	private:
		TArray<TWeakObjectPtr<UPackage>> PackagesToResetLoaders;
		TArray<TWeakObjectPtr<UPackage>> PackagesToReload;
		FString ConfirmedClosureSignature;
		int32 OwnerPackageReloadCount = 0;
		bool bClosureConfirmed = false;
		bool bPrepared = false;
		bool bLoadersReset = false;
	};

	/** 无全局 Source Control provider 的固定 HEAD 精确 Revert service. */
	class GITSOURCECONTROL_API FGitChangedAssetOperations final
	{
	public:
		FGitChangedAssetOperations(FString InGitBinary, FString InRepositoryRoot);

		/**
		 * 调用方必须传入 snapshot 捕获的 complete immutable HEAD SHA. 此方法会在
		 * transaction mutex 内反复验证 HEAD/index/fingerprint, 并且不会使用 git clean.
		 */
		bool RevertToHead(const FString& InPinnedHead, const TArray<FGitChangedAssetEntry>& InEntries,
			const FGitChangedAssetRevertCallbacks& InCallbacks, FGitChangedAssetRevertResult& OutResult) const;

		/** Fast, UI-independent eligibility guard. 不能替代 metadata resolver 的 owner 检查. */
		static bool ValidateEntries(const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError);

	private:
		FString GitBinary;
		FString RepositoryRoot;
	};
}
