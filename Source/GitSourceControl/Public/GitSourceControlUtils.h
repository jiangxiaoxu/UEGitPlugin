// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlHistoryMode.h"
#include "GitSourceControlFileStatus.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION == 5
#include "UObject/ObjectSaveContext.h"
#endif

/**
 * Helper struct for maintaining temporary files for passing to commands
 */
class FGitScopedTempFile
{
public:

	/** Constructor - open & write string to temp file */
	FGitScopedTempFile(const FText& InText);

	/** Destructor - delete temp file */
	~FGitScopedTempFile();

	/** Get the filename of this temp file - empty if it failed to be created */
	const FString& GetFilename() const;

private:
	/** The filename we are writing to */
	FString Filename;
};

class UPackage;

#if WITH_DEV_AUTOMATION_TESTS
struct GITSOURCECONTROL_API FGitStandaloneHistoryTestEntry
{
	FString CommitId;
	FString HistoricalPath;
	FString LocalFilename;
	FString Description;
	FString Author;
	FString Action;
};
#endif

struct GITSOURCECONTROL_API FGitRenamePair
{
	FString OldPath;
	FString NewPath;
};

/** Raw `git update-index -z --index-info` records for one explicit set of paths. */
struct GITSOURCECONTROL_API FGitIndexSnapshot
{
	FString RepositoryRoot;
	TArray<FString> Paths;
	TArray<uint8> IndexInfo;
};

namespace GitSourceControlUtils
{
	/** Frozen outcome of the single Git executable probe performed when the plugin starts. */
	enum class EGitStartupCapabilityState : uint8
	{
		Pending,
		Available,
		Unavailable,
	};

	/**
	 * Session-scoped Git executable capability. The result never re-runs discovery:
	 * install, removal, or upgrade requires an Editor restart.
	 */
	struct GITSOURCECONTROL_API FGitStartupCapability
	{
		EGitStartupCapabilityState State = EGitStartupCapabilityState::Unavailable;
		FString GitBinary;
		FString Diagnostic;
	};

	/** 跨 UI 和 worker 传递的取消边界. 只读和网络 Git 工作可在任意时刻取消. */
	class GITSOURCECONTROL_API FGitOperationCancellationContext final
	{
	public:
		void Cancel();
		bool IsCancellationRequested() const;

	private:
		TAtomic<bool> bCancellationRequested = false;
	};

	/**
	 * 将一个取消边界绑定到当前执行链. Scope 会保留嵌套绑定并传播给
	 * 由 Git utility 发起的内部 worker, 使一个 UI Cancel 可以终止全部只读 Git 子进程.
	 */
	class GITSOURCECONTROL_API FGitOperationCancellationScope final
	{
	public:
		explicit FGitOperationCancellationScope(TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> InContext);
		~FGitOperationCancellationScope();

		FGitOperationCancellationScope(const FGitOperationCancellationScope&) = delete;
		FGitOperationCancellationScope& operator=(const FGitOperationCancellationScope&) = delete;

	private:
		TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> Context;
		TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> PreviousContext;
	};

	/** Mark the session capability as pending before the module launches its one startup probe. Returns false after the one-shot probe has begun. */
	GITSOURCECONTROL_API bool BeginStartupGitCapabilityProbe();

	/** Run Git executable discovery/version validation off the GameThread. */
	GITSOURCECONTROL_API FGitStartupCapability ProbeStartupGitCapability();

	/** Freeze the pending capability with the completed startup probe result. */
	GITSOURCECONTROL_API void CompleteStartupGitCapabilityProbe(FGitStartupCapability InCapability);

	/** Return the immutable session capability snapshot. */
	GITSOURCECONTROL_API FGitStartupCapability GetStartupGitCapability();

	/** True only when the startup probe found Git 2.53.0 or newer. */
	GITSOURCECONTROL_API bool IsStartupGitCapabilityAvailable();

	/** User-facing explanation for Pending/Unavailable capability states. */
	GITSOURCECONTROL_API FText GetStartupGitCapabilityMessage();

	/**
		*  Returns an updated repo root if all selected files are in a plugin subfolder, and the plugin subfolder is a git repo
		*  This supports the case where each plugin is a sub module
		*
		* @param AbsoluteFilePaths		The list of files in the SC operation
		* @param PathToRepositoryRoot	The original path to the repository root (used by default)
		*/
	FString ChangeRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths, const FString& PathToRepositoryRoot);

	/**
		*  Returns an updated repo root if all selected file is in a plugin subfolder, and the plugin subfolder is a git repo
		*  This supports the case where each plugin is a sub module
		*
		* @param AbsoluteFilePath		The file in the SC operation
		* @param PathToRepositoryRoot	The original path to the repository root (used by default)
		*/
	FString ChangeRepositoryRootIfSubmodule(FString & AbsoluteFilePath, const FString& PathToRepositoryRoot);

/** Return the Git binary from the frozen startup capability snapshot, if available. Never performs discovery. */
GITSOURCECONTROL_API FString FindGitBinaryPath();

/** Resolve a local Git executable and repository root for one explicit workspace file. */
GITSOURCECONTROL_API bool ResolveStandaloneRepositoryForFile(const FString& InFilename, FString& OutGitBinary, FString& OutRepositoryRoot, FString& OutError);

/** Verify local-only porcelain-v2 status and literal-path query capabilities for explicit standalone mutations. */
GITSOURCECONTROL_API bool CheckLocalGitCapabilities(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutError);

/** Changed Assets 使用的固定全仓 porcelain-v2 status. Git 使用 NUL record separator, 故保持 bytes; 不接受调用方 Git 参数. */
GITSOURCECONTROL_API bool RunRepositoryStatusPorcelainV2(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
	TArray<uint8>& OutStandardOutput, FString& OutError);

/** Changed Assets transaction recheck 使用的固定 literal-pathspec porcelain-v2 status; 保留 NUL 输出. */
GITSOURCECONTROL_API bool RunPathsStatusPorcelainV2(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
	const TArray<FString>& InFiles, TArray<uint8>& OutStandardOutput, FString& OutError);

/** Drop a cached Git binary after an external launcher observes a launch failure. */
GITSOURCECONTROL_API void InvalidateVerifiedGitBinary(const FString& InPathToGitBinary);

/**
 * Find the root of the Git repository, looking from the provided path and upward in its parent directories
 * @param InPath				The path to the Game Directory (or any path or file in any git repository)
 * @param OutRepositoryRoot		The path to the root directory of the Git repository if found, else the path to the ProjectDir
 * @returns true if the command succeeded and returned no errors
 */
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot);

/**
 * Get Git config user.name & user.email
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	OutUserName			Name of the Git user configured for this repository (or globaly)
 * @param	OutEmailName		E-mail of the Git user configured for this repository (or globaly)
 */
void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail);


/**
 * Run a Git command - output is a string TArray.
 *
 * @param	InCommand			The Git command - e.g. commit
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	InParameters		The parameters to the Git command
 * @param	InFiles				The files to be operated on
 * @param	OutResults			The results (from StdOut) as an array per-line
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @returns true if the command succeeded and returned no errors
 */
GITSOURCECONTROL_API  bool RunCommand( const FString & InCommand, const FString & InPathToGitBinary, const FString & InRepositoryRoot, const TArray< FString > & InParameters, const TArray< FString > & InFiles, TArray< FString > & OutResults, TArray< FString > & OutErrorMessages );
bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode = 0, const bool bLogFailure = true);

/**
 * Unloads packages of specified named files
 */
TArray<class UPackage*> UnlinkPackages(const TArray<FString>& InPackageNames);

/**
 * Reloads packages for these packages
 */
void ReloadPackages(TArray<UPackage*>& InPackagesToReload);

/**
 * Run a Git "status" command and parse it.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	InUsingLfsLocking	Tells if using the Git LFS file Locking workflow
 * @param	InFiles				The files to be operated on
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @param   OutStates           The resultant states
 * @returns true if the command succeeded and returned no errors
 */
GITSOURCECONTROL_API bool RunUpdateStatus(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
					 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlFileStatus>& OutStates);

/** Expand selected paths with their porcelain-v2 rename counterpart as atomic old/new groups. */
GITSOURCECONTROL_API bool ExpandSelectedPathsWithRenamePairs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InSelectedFiles,
	TArray<FString>& OutExpandedFiles, TArray<FGitRenamePair>& OutRenamePairs, TArray<FString>& OutErrorMessages);

/** Capture stage entries, including explicit removal records for selected paths absent from the index. */
GITSOURCECONTROL_API bool CaptureIndexEntriesForPaths(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles,
	FGitIndexSnapshot& OutSnapshot, FString& OutError);

/** Restore only entries in a prior snapshot through stdin consumed by `git update-index -z --index-info`. */
GITSOURCECONTROL_API bool RestoreIndexEntries(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FGitIndexSnapshot& InSnapshot, FString& OutError);

/** Run a guarded local restore or exact-path index reset; no directory or repository-wide mutation is accepted. */
GITSOURCECONTROL_API bool RunExactPathspecMutation(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InVerb,
	const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutError);

/**
 * Export an exact Git blob without filters or LFS smudging, then atomically replace OutputFile.
 * The command is local-only and never performs network I/O.
 */
GITSOURCECONTROL_API bool DumpRevisionBlobToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InRevisionSpec, const FString& InOutputFile, FString& OutError);

/**
 * Verify a local Git LFS object against its pointer metadata using the resolved
 * Git binary's local `git lfs pointer` command. Git LFS 3.7.1+ is required and
 * lazily capability-cached on the first explicit LFS operation. This never invokes fetch or transfer.
 */
	GITSOURCECONTROL_API bool VerifyLocalLfsObject(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InObjectFilename, const FString& InExpectedOid, int64 InExpectedSize, FString& OutError);

	/**
	 * Fetch one historical LFS path only after an explicit History Diff/Restore request.
	 * The remote is resolved as current branch upstream, then the sole configured remote.
	 */
	GITSOURCECONTROL_API bool FetchLfsContentForRevision(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
		const FString& InFullCommitId, const FString& InHistoricalPath, FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	namespace Testing
	{
		GITSOURCECONTROL_API void ResetGitProcessLaunchCount();
		GITSOURCECONTROL_API void ResetVerifiedGitBinaryCache();
		GITSOURCECONTROL_API uint64 GetGitProcessLaunchCount();
		GITSOURCECONTROL_API uint64 GetGitLfsFetchLaunchCount();
		GITSOURCECONTROL_API uint64 GetGitProcessLaunchCountAtModuleStartup();
		GITSOURCECONTROL_API void CaptureGitProcessLaunchCountAtModuleStartup();
		GITSOURCECONTROL_API uint32 GetStartupGitCapabilityProbeCount();
		GITSOURCECONTROL_API void SetStartupGitCapabilityForTesting(const FGitStartupCapability& InCapability);
		GITSOURCECONTROL_API bool LoadStandaloneHistory(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InFilename,
			EGitLocalSourceControlHistoryMode InMode, FString& OutCapturedHead, bool& bOutHeadChanged, TArray<FGitStandaloneHistoryTestEntry>& OutHistory, FString& OutError);
		GITSOURCECONTROL_API bool ExportStandaloneRevisionForDiff(const FString& InGitBinary, const FString& InRepositoryRoot,
			const FString& InLocalFilename, const FString& InCommitId, const FString& InHistoricalPath, FString& OutTempFilename);
		GITSOURCECONTROL_API UPackage* LoadStandaloneRevisionPackageForDiff(const FString& InGitBinary, const FString& InRepositoryRoot,
			const FString& InLocalFilename, const FString& InCommitId, const FString& InHistoricalPath);
	}
#endif

/**
 * Run a Git "log" command and parse it.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	InFile				The file to be operated on
 * @param	bMergeConflict		merge conflict 时 history 不可用
 * @param	InMode				仅当前路径或已提交 R100 rename 链
 * @param	OutCapturedHead		查询前解析的 HEAD; 所有 entry 使用这个 immutable snapshot
 * @param	bOutHeadChanged		immutable query 运行期间 HEAD 变化时为 true
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @param	OutHistory			The history of the file
 */

/**
 * Helper function to convert a filename array to relative paths.
 * @param	InFileNames		The filename array
 * @param	InRelativeTo	Path to the WorkspaceRoot
 * @return an array of filenames, transformed into relative paths
 */
TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo);

/**
 * Helper function to convert a filename array to absolute paths.
 * @param	InFileNames		The filename array (relative paths)
 * @param	InRelativeTo	Path to the WorkspaceRoot
 * @return an array of filenames, transformed into absolute paths
 */
TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo);

}
