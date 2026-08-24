// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlRevision.h"
#include "GitSourceControlState.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION == 5
#include "UObject/ObjectSaveContext.h"
#endif

class FGitSourceControlState;

class FGitSourceControlCommand;

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

struct FGitVersion;

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

/**
 * Find the path to the Git binary, looking into a few places (standalone Git install, and other common tools embedding Git)
 * @returns the path to the Git binary if found, or an empty string.
 */
GITSOURCECONTROL_API FString FindGitBinaryPath();

/**
 * Run a Git "version" command to check the availability of the binary.
 * @param InPathToGitBinary		The path to the Git binary
 * @param OutGitVersion         If provided, populate with the git version parsed from "version" command
 * @returns true if the command succeeded and returned no errors
 */
bool CheckGitAvailability(const FString& InPathToGitBinary, FGitVersion* OutVersion = nullptr);

/** Verify the local-only porcelain-v2 status and literal path query capabilities required by this provider. */
GITSOURCECONTROL_API bool CheckLocalGitCapabilities(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutError);

/**
 * Parse the output from the "version" command into GitMajorVersion and GitMinorVersion.
 * @param InVersionString       The version string returned by `git --version`
 * @param OutVersion            The FGitVersion to populate
 */
 void ParseGitVersion(const FString& InVersionString, FGitVersion* OutVersion);

	/**
		* Check git for various optional capabilities by various means.
		* @param InPathToGitBinary		The path to the Git binary
		* @param OutGitVersion			If provided, populate with the git version parsed from "version" command
		*/
	void FindGitCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion);

	/**
		* Run a Git "lfs" command to check the availability of the "Large File System" extension.
		* @param InPathToGitBinary		The path to the Git binary
		* @param OutGitVersion			If provided, populate with the git version parsed from "version" command
		*/
	void FindGitLfsCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion);

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

/** Bind cancellation to Git work executed by the current worker thread. */
void SetActiveCommand(const FGitSourceControlCommand* InCommand);

/** Clear the cancellation binding for the current worker thread. */
void ClearActiveCommand(const FGitSourceControlCommand* InCommand);

/** Increment the local repository generation after an external or local mutation. */
GITSOURCECONTROL_API void InvalidateRepository(const FString& InRepositoryRoot);

/** Return the local repository generation used to reject stale asynchronous results. */
GITSOURCECONTROL_API uint64 GetRepositoryGeneration(const FString& InRepositoryRoot);

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
					 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates);

/** Expand selected paths with their porcelain-v2 rename counterpart as atomic old/new groups. */
GITSOURCECONTROL_API bool ExpandSelectedPathsWithRenamePairs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InSelectedFiles,
	TArray<FString>& OutExpandedFiles, TArray<FGitRenamePair>& OutRenamePairs, TArray<FString>& OutErrorMessages);

/** Capture stage entries, including explicit removal records for selected paths absent from the index. */
GITSOURCECONTROL_API bool CaptureIndexEntriesForPaths(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles,
	FGitIndexSnapshot& OutSnapshot, FString& OutError);

/** Restore only entries in a prior snapshot through stdin consumed by `git update-index -z --index-info`. */
GITSOURCECONTROL_API bool RestoreIndexEntries(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FGitIndexSnapshot& InSnapshot, FString& OutError);

/** Run a guarded local restore on explicit absolute files; no directory or repository-wide mutation is accepted. */
GITSOURCECONTROL_API bool RunExactPathspecMutation(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InVerb,
	const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutError);

/**
 * Export an exact Git blob without filters or LFS smudging, then atomically replace OutputFile.
 * The command is local-only and never performs network I/O.
 */
GITSOURCECONTROL_API bool DumpRevisionBlobToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InRevisionSpec, const FString& InOutputFile, FString& OutError);

/**
 * Verify a local Git LFS object against its pointer metadata using the resolved
 * Git binary's local `git lfs pointer` command. This never invokes fetch or transfer.
 */
	GITSOURCECONTROL_API bool VerifyLocalLfsObject(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InObjectFilename, const FString& InExpectedOid, int64 InExpectedSize, FString& OutError);

	/**
	 * Fetch one historical LFS path only after an explicit History Diff/Restore request.
	 * The remote is resolved as current upstream, then origin, then the sole configured remote.
	 */
	GITSOURCECONTROL_API bool FetchLfsContentForRevision(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
		const FString& InFullCommitId, const FString& InHistoricalPath, FString& OutError);

/**
 * Run a Git "log" command and parse it.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	InFile				The file to be operated on
 * @param	bMergeConflict		In case of a merge conflict, we also need to get the tip of the "remote branch" (MERGE_HEAD) before the log of the "current branch" (HEAD)
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @param	OutHistory			The history of the file
 */
GITSOURCECONTROL_API bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict, TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory);

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

/**
 * Remove redundant errors (that contain a particular string) and also
 * update the commands success status if all errors were removed.
 */
void RemoveRedundantErrors(FGitSourceControlCommand& InCommand, const FString& InFilter);


/**
 * Helper function for various commands to update cached states.
 * @returns true if any states were updated
 */
GITSOURCECONTROL_API bool UpdateCachedStates( const TMap< const FString, FGitState > & InResults );

/**
* Helper function for various commands to collect new states.
* @returns true if any states were updated
*/
GITSOURCECONTROL_API bool CollectNewStates( const TMap< FString, FGitSourceControlState > & InStates, TMap< const FString, FGitState > & OutResults );
	
/**
 * Helper function for various commands to collect new states.
 * @returns true if any states were updated
 */
bool CollectNewStates(const TArray<FString>& InFiles, TMap<const FString, FGitState>& OutResults, EFileState::Type FileState, ETreeState::Type TreeState = ETreeState::Unset, ELockState::Type LockState = ELockState::Unset, ERemoteState::Type RemoteState = ERemoteState::Unset);

}
