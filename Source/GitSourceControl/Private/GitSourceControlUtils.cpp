// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlUtils.h"

#include "Algo/AllOf.h"
#include "DiffUtils.h"
#include "GitLfsLocalObjectStore.h"
#include "GitSourceControlRevision.h"
#include "GitStandaloneHistory.h"
#include "GitStandaloneLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#include "HAL/PlatformFile.h"
#if ENGINE_MAJOR_VERSION >= 5
#include "HAL/PlatformFileManager.h"
#else
#include "HAL/PlatformFilemanager.h"
#endif

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Misc/Timespan.h"

#include "PackageTools.h"
#include "FileHelpers.h"

#include "Runtime/Launch/Resources/Version.h"
#include "Async/Async.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

FGitScopedTempFile::FGitScopedTempFile(const FText& InText)
{
	Filename = FPaths::CreateTempFilename(*FPaths::ProjectLogDir(), TEXT("Git-Temp"), TEXT(".txt"));
	if (!FFileHelper::SaveStringToFile(InText.ToString(), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogGitStandalone, Error, TEXT("Failed to write to temp file: %s"), *Filename);
	}
}

FGitScopedTempFile::~FGitScopedTempFile()
{
	if (FPaths::FileExists(Filename))
	{
		if (!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
		{
			UE_LOG(LogGitStandalone, Error, TEXT("Failed to delete temp file: %s"), *Filename);
		}
	}
}

const FString& FGitScopedTempFile::GetFilename() const
{
	return Filename;
}

namespace GitSourceControlUtils
{
	FString ChangeRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths, const FString& PathToRepositoryRoot)
	{
		FString Ret = PathToRepositoryRoot;
		// note this is not going to support operations where selected files are in different repositories

		TArray<FString> PackageNotIncludedInGit;
		PackageNotIncludedInGit.Reserve(AbsoluteFilePaths.Num());

		for (auto& FilePath : AbsoluteFilePaths)
		{
			FString TestPath = FilePath;
			while (!FPaths::IsSamePath(TestPath, PathToRepositoryRoot))
			{
				// Iterating over path directories, looking for .git
				TestPath = FPaths::GetPath(TestPath);

				if (TestPath.IsEmpty())
				{
					// TestPath.IsEmpty() meaning is that FilePath is not git file. So it need to removed to git command file list.
					PackageNotIncludedInGit.Add(FilePath);
					UE_LOG(LogGitStandalone, Warning, TEXT("Package file to update has included dependent file is not git or Can't find directory path for file : %s"), *FilePath);

					break;
				}
				
				FString GitTestPath = TestPath + "/.git";
				if (FPaths::FileExists(GitTestPath) || FPaths::DirectoryExists(GitTestPath))
				{
					FString RetNormalized = Ret;
					FPaths::NormalizeDirectoryName(RetNormalized);
					FString PathToRepositoryRootNormalized = PathToRepositoryRoot;
					FPaths::NormalizeDirectoryName(PathToRepositoryRootNormalized);
					if (!FPaths::IsSamePath(RetNormalized, PathToRepositoryRootNormalized) && Ret != FPaths::GetPath(GitTestPath))
					{
						UE_LOG(LogGitStandalone, Error, TEXT("Selected files belong to different submodules"));
						return PathToRepositoryRoot;
					}
					Ret = TestPath;
					break;
				}
			}
		}
#if ENGINE_MAJOR_VERSION >= 5
		if (!PackageNotIncludedInGit.IsEmpty())
#else
		if (PackageNotIncludedInGit.Num() > 0)
#endif
		{
			for (const FString& ToRemoveFile : PackageNotIncludedInGit)
			{
				AbsoluteFilePaths.Remove(ToRemoveFile);
			}
		}

		return Ret;
	}

	FString ChangeRepositoryRootIfSubmodule(FString & AbsoluteFilePath, const FString& PathToRepositoryRoot)
	{
		TArray<FString> AbsoluteFilePaths = { AbsoluteFilePath };
		return ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);
	}

namespace GitSourceControlUtilsPrivate
{
constexpr double GitCommandTimeoutSeconds = 30.0;
constexpr double GitNetworkCommandTimeoutSeconds = 90.0;
constexpr int32 MinimumGitMajorVersion = 2;
constexpr int32 MinimumGitMinorVersion = 53;
constexpr int32 MinimumGitPatchVersion = 0;
constexpr int32 MinimumGitLfsMajorVersion = 3;
constexpr int32 MinimumGitLfsMinorVersion = 7;
constexpr int32 MinimumGitLfsPatchVersion = 1;

#if WITH_DEV_AUTOMATION_TESTS
TAtomic<uint64> GitProcessLaunchCount = 0;
TAtomic<uint64> GitLfsFetchLaunchCount = 0;
TAtomic<uint64> GitProcessLaunchCountAtModuleStartup = MAX_uint64;
#endif

const TArray<FString>& GetEmptyStringArray()
{
	static const TArray<FString> Empty;
	return Empty;
}

struct FGitProcessResult
{
	int32 ReturnCode = -1;
	bool bLaunchFailed = false;
	bool bCancelled = false;
	bool bTimedOut = false;
	bool bInputWriteFailed = false;
	TArray<uint8> StandardOutput;
	TArray<uint8> StandardError;
};

struct FGitReleaseVersion
{
	int32 Major = 0;
	int32 Minor = 0;
	int32 Patch = 0;
};

struct FGitBinaryCapabilityCache
{
	FString NormalizedPath;
	FGitReleaseVersion Version;
	bool bIsValid = false;
};

struct FGitLfsCapabilityCache
{
	FString NormalizedPath;
	bool bIsSupported = false;
};

FCriticalSection GitRepositoryGatesLock;
TMap<FString, TSharedRef<FCriticalSection, ESPMode::ThreadSafe>> GitRepositoryGates;
FCriticalSection GitBinaryCapabilityCacheLock;
FGitBinaryCapabilityCache GitBinaryCapabilityCache;
FGitLfsCapabilityCache GitLfsCapabilityCache;
thread_local TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> ActiveGitCancellationContext;

FString NormalizeRepositoryKey(const FString& InRepositoryRoot)
{
	FString Result = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(Result);
#if PLATFORM_WINDOWS
	Result = Result.ToLower();
#endif
	return Result;
}

FString NormalizeGitBinaryPath(const FString& InPathToGitBinary)
{
	FString Result = FPaths::ConvertRelativePathToFull(InPathToGitBinary);
	FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
	Result.ToLowerInline();
#endif
	return Result;
}

void InvalidateVerifiedGitBinaryInternal(const FString& InPathToGitBinary)
{
	const FString NormalizedPath = NormalizeGitBinaryPath(InPathToGitBinary);
	if (NormalizedPath.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&GitBinaryCapabilityCacheLock);
	if (GitBinaryCapabilityCache.bIsValid && GitBinaryCapabilityCache.NormalizedPath == NormalizedPath)
	{
		GitBinaryCapabilityCache = FGitBinaryCapabilityCache();
	}
	if (GitLfsCapabilityCache.bIsSupported && GitLfsCapabilityCache.NormalizedPath == NormalizedPath)
	{
		GitLfsCapabilityCache = FGitLfsCapabilityCache();
	}
}

bool TryGetVerifiedGitBinary(FString& OutGitBinary)
{
	OutGitBinary.Reset();
	FScopeLock Lock(&GitBinaryCapabilityCacheLock);
	if (!GitBinaryCapabilityCache.bIsValid)
	{
		return false;
	}
	if (!FPaths::FileExists(GitBinaryCapabilityCache.NormalizedPath))
	{
		GitBinaryCapabilityCache = FGitBinaryCapabilityCache();
		return false;
	}
	OutGitBinary = GitBinaryCapabilityCache.NormalizedPath;
	FPaths::MakePlatformFilename(OutGitBinary);
	return true;
}

void CacheVerifiedGitBinary(const FString& InPathToGitBinary, const FGitReleaseVersion& InVersion)
{
	const FString NormalizedPath = NormalizeGitBinaryPath(InPathToGitBinary);
	if (NormalizedPath.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&GitBinaryCapabilityCacheLock);
	GitBinaryCapabilityCache.NormalizedPath = NormalizedPath;
	GitBinaryCapabilityCache.Version = InVersion;
	GitBinaryCapabilityCache.bIsValid = true;
}

bool MakeRepositoryRelativePath(const FString& InRepositoryRoot, FString& InOutPath)
{
	FString CanonicalRepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(CanonicalRepositoryRoot);
	if (CanonicalRepositoryRoot.IsEmpty())
	{
		return false;
	}

	// FPaths::MakePathRelativeTo treats a root without a trailing separator as
	// the parent directory and can strip the repository directory itself.
	if (!CanonicalRepositoryRoot.EndsWith(TEXT("/")))
	{
		CanonicalRepositoryRoot += TEXT("/");
	}

	FString CanonicalPath = FPaths::ConvertRelativePathToFull(InOutPath);
	FPaths::NormalizeFilename(CanonicalPath);
	if (!FPaths::IsUnderDirectory(CanonicalPath, CanonicalRepositoryRoot))
	{
		return false;
	}
	if (!FPaths::MakePathRelativeTo(CanonicalPath, *CanonicalRepositoryRoot))
	{
		return false;
	}
	FPaths::NormalizeFilename(CanonicalPath);
	if (CanonicalPath.IsEmpty() || CanonicalPath == TEXT(".") || CanonicalPath == TEXT("..") ||
		CanonicalPath.StartsWith(TEXT("../"), ESearchCase::CaseSensitive))
	{
		return false;
	}

	InOutPath = MoveTemp(CanonicalPath);
	return true;
}

TSharedRef<FCriticalSection, ESPMode::ThreadSafe> GetRepositoryGate(const FString& InRepositoryRoot)
{
	const FString Key = NormalizeRepositoryKey(InRepositoryRoot);
	FScopeLock Lock(&GitRepositoryGatesLock);
	if (const TSharedRef<FCriticalSection, ESPMode::ThreadSafe>* ExistingGate = GitRepositoryGates.Find(Key))
	{
		return *ExistingGate;
	}

	TSharedRef<FCriticalSection, ESPMode::ThreadSafe> NewGate = MakeShared<FCriticalSection, ESPMode::ThreadSafe>();
	GitRepositoryGates.Add(Key, NewGate);
	return NewGate;
}

bool IsGitOperationCancelled()
{
	return ActiveGitCancellationContext.IsValid() && ActiveGitCancellationContext->IsCancellationRequested();
}

FString BytesToString(const TArray<uint8>& InBytes)
{
	FString Result;
	if (InBytes.Num() > 0)
	{
		FFileHelper::BufferToString(Result, InBytes.GetData(), InBytes.Num());
	}
	return Result;
}

void DrainPipe(void* InPipe, TArray<uint8>& OutBytes)
{
	TArray<uint8> Buffer;
	while (FPlatformProcess::ReadPipeToArray(InPipe, Buffer))
	{
		if (Buffer.Num() == 0)
		{
			break;
		}
		OutBytes.Append(Buffer);
		Buffer.Reset();
	}
}

FGitProcessResult ExecuteGitProcess(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InArguments,
	const TArray<uint8>* InStandardInput = nullptr, const double InTimeoutSeconds = GitCommandTimeoutSeconds)
{
	FGitProcessResult Result;
	const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> RepositoryGate = GetRepositoryGate(InRepositoryRoot);
	bool bRepositoryGateLocked = false;
	ON_SCOPE_EXIT
	{
		if (bRepositoryGateLocked)
		{
			RepositoryGate->Unlock();
		}
	};
	while (!RepositoryGate->TryLock())
	{
		if (IsGitOperationCancelled())
		{
			Result.bCancelled = true;
			return Result;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	bRepositoryGateLocked = true;
	if (IsGitOperationCancelled())
	{
		Result.bCancelled = true;
		return Result;
	}

	void* StandardOutputRead = nullptr;
	void* StandardOutputWrite = nullptr;
	void* StandardErrorRead = nullptr;
	void* StandardErrorWrite = nullptr;
	void* StandardInputRead = nullptr;
	void* StandardInputWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(StandardOutputRead, StandardOutputWrite) || !FPlatformProcess::CreatePipe(StandardErrorRead, StandardErrorWrite))
	{
		if (StandardOutputRead || StandardOutputWrite)
		{
			FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		}
		return Result;
	}
	if (InStandardInput != nullptr && !FPlatformProcess::CreatePipe(StandardInputRead, StandardInputWrite, true))
	{
		FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
		return Result;
	}

	FProcHandle ProcessHandle = FPlatformProcess::CreateProc(
		*InPathToGitBinary,
		*InArguments,
		false,
		true,
		true,
		nullptr,
		0,
		InRepositoryRoot.IsEmpty() ? nullptr : *InRepositoryRoot,
		StandardOutputWrite,
		StandardInputRead,
		StandardErrorWrite);

	if (!ProcessHandle.IsValid())
	{
		Result.bLaunchFailed = true;
		InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
		FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
		if (StandardInputRead || StandardInputWrite)
		{
			FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
		}
		return Result;
	}

#if WITH_DEV_AUTOMATION_TESTS
	++GitProcessLaunchCount;
#endif

	if (InStandardInput != nullptr)
	{
		int32 BytesWritten = 0;
		if (!FPlatformProcess::WritePipe(StandardInputWrite, InStandardInput->GetData(), InStandardInput->Num(), &BytesWritten) || BytesWritten != InStandardInput->Num())
		{
			Result.bInputWriteFailed = true;
			FPlatformProcess::TerminateProc(ProcessHandle, true);
		}
		FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
		StandardInputRead = nullptr;
		StandardInputWrite = nullptr;
	}

	const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(InTimeoutSeconds);
	while (FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		DrainPipe(StandardOutputRead, Result.StandardOutput);
		DrainPipe(StandardErrorRead, Result.StandardError);
		if (IsGitOperationCancelled())
		{
			Result.bCancelled = true;
			FPlatformProcess::TerminateProc(ProcessHandle, true);
			break;
		}
		if (FDateTime::UtcNow() >= Deadline)
		{
			Result.bTimedOut = true;
			FPlatformProcess::TerminateProc(ProcessHandle, true);
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}

	DrainPipe(StandardOutputRead, Result.StandardOutput);
	DrainPipe(StandardErrorRead, Result.StandardError);
	FPlatformProcess::GetProcReturnCode(ProcessHandle, &Result.ReturnCode);
	FPlatformProcess::CloseProc(ProcessHandle);
	FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
	FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
	if (StandardInputRead || StandardInputWrite)
	{
		FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
	}
	return Result;
}

FGitProcessResult ExecuteGitProcessOffGameThread(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InArguments,
	const double InTimeoutSeconds = GitCommandTimeoutSeconds)
{
	if (!IsInGameThread())
	{
		return ExecuteGitProcess(InPathToGitBinary, InRepositoryRoot, InArguments, nullptr, InTimeoutSeconds);
	}

	const TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = ActiveGitCancellationContext;
	return Async(EAsyncExecution::ThreadPool, [InPathToGitBinary, InRepositoryRoot, InArguments, InTimeoutSeconds, CancellationContext]()
	{
		ActiveGitCancellationContext = CancellationContext;
		FGitProcessResult Result = ExecuteGitProcess(InPathToGitBinary, InRepositoryRoot, InArguments, nullptr, InTimeoutSeconds);
		ActiveGitCancellationContext.Reset();
		return Result;
	}).Get();
}

FGitProcessResult ExecuteGitProcessOffGameThreadWithInput(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InArguments, const TArray<uint8>& InStandardInput)
{
	if (!IsInGameThread())
	{
		return ExecuteGitProcess(InPathToGitBinary, InRepositoryRoot, InArguments, &InStandardInput);
	}

	const TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = ActiveGitCancellationContext;
	return Async(EAsyncExecution::ThreadPool, [InPathToGitBinary, InRepositoryRoot, InArguments, StandardInput = InStandardInput, CancellationContext]()
	{
		ActiveGitCancellationContext = CancellationContext;
		FGitProcessResult Result = ExecuteGitProcess(InPathToGitBinary, InRepositoryRoot, InArguments, &StandardInput);
		ActiveGitCancellationContext.Reset();
		return Result;
	}).Get();
}

FString QuoteGitArgument(const FString& InArgument)
{
	FString Result;
	Result.Reserve(InArgument.Len() + 2);
	Result.AppendChar(TEXT('"'));

	int32 ConsecutiveBackslashes = 0;
	for (const TCHAR Character : InArgument)
	{
		if (Character == TEXT('\\'))
		{
			++ConsecutiveBackslashes;
			continue;
		}

		if (Character == TEXT('"'))
		{
			Result += FString::ChrN(ConsecutiveBackslashes * 2 + 1, TEXT('\\'));
			Result.AppendChar(TEXT('"'));
			ConsecutiveBackslashes = 0;
			continue;
		}

		if (ConsecutiveBackslashes > 0)
		{
			Result += FString::ChrN(ConsecutiveBackslashes, TEXT('\\'));
			ConsecutiveBackslashes = 0;
		}
		Result.AppendChar(Character);
	}

	// Backslashes immediately before the closing quote must be doubled for the
	// Windows command-line parser. This representation is also accepted by the
	// platform process implementations used by the other supported hosts.
	Result += FString::ChrN(ConsecutiveBackslashes * 2, TEXT('\\'));
	Result.AppendChar(TEXT('"'));
	return Result;
}

void AppendGitArgument(FString& InOutArguments, const FString& InArgument)
{
	if (!InOutArguments.IsEmpty())
	{
		InOutArguments.AppendChar(TEXT(' '));
	}
	InOutArguments += QuoteGitArgument(InArgument);
}

bool ParseLfsPointerOutput(const FString& InOutput, FString& OutOid, int64& OutSize)
{
	OutOid.Reset();
	OutSize = 0;
	FTCHARToUTF8 Utf8(*InOutput);
	TArray<uint8> Data;
	Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	FGitLfsPointer Pointer;
	if (ParseGitLfsPointer(Data, Pointer) != EGitLfsPointerParseResult::ValidPointer)
	{
		return false;
	}
	OutOid = MoveTemp(Pointer.Oid);
	OutSize = Pointer.Size;
	return true;
}

bool IsFullGitCommitId(const FString& InCommitId)
{
	if (InCommitId.Len() != 40 && InCommitId.Len() != 64)
	{
		return false;
	}
	return Algo::AllOf(InCommitId, [](const TCHAR Character)
	{
		return FChar::IsHexDigit(Character);
	});
}

bool NormalizeHistoricalLfsPath(const FString& InHistoricalPath, FString& OutPath)
{
	// `git lfs fetch --include` accepts gitignore-style patterns, not a literal
	// path.  Historical revision fetches must never widen their scope, so keep
	// the input slash-normalized and reject every pattern/control character
	// before it reaches Git.
	OutPath = InHistoricalPath;
	if (OutPath.IsEmpty() || OutPath.StartsWith(TEXT("/")) || OutPath.EndsWith(TEXT("/")) || OutPath.Contains(TEXT("//")))
	{
		return false;
	}
	for (const TCHAR Character : OutPath)
	{
		if (FChar::IsControl(Character) || Character == TEXT(',') || Character == TEXT('*') || Character == TEXT('?') ||
			Character == TEXT('[') || Character == TEXT(']') || Character == TEXT('!') || Character == TEXT('#') ||
			Character == TEXT('\\') || Character == TEXT(':') || Character == TEXT('\r') || Character == TEXT('\n'))
		{
			return false;
		}
	}
	TArray<FString> Segments;
	OutPath.ParseIntoArray(Segments, TEXT("/"), true);
	for (const FString& Segment : Segments)
	{
		if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
		{
			return false;
		}
	}
	return Segments.Num() > 0;
}

bool FindConfiguredRemote(const TArray<FString>& InRemotes, const FString& InCandidate, FString& OutRemote)
{
	for (const FString& Remote : InRemotes)
	{
		if (Remote.Equals(InCandidate, ESearchCase::CaseSensitive))
		{
			OutRemote = Remote;
			return true;
		}
	}
	return false;
}

bool ResolveLfsFetchRemote(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemote, FString& OutError)
{
	OutRemote.Reset();
	OutError.Reset();
	FString RemoteOutput;
	FString RemoteErrors;
	if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("remote"), InPathToGitBinary, InRepositoryRoot,
		GetEmptyStringArray(), GetEmptyStringArray(), RemoteOutput, RemoteErrors))
	{
		OutError = RemoteErrors.IsEmpty() ? TEXT("Could not list Git remotes for Git LFS download.") : RemoteErrors;
		return false;
	}

	TArray<FString> Remotes;
	RemoteOutput.ParseIntoArrayLines(Remotes, true);
	for (FString& Remote : Remotes)
	{
		Remote.TrimStartAndEndInline();
	}
	Remotes.RemoveAll([](const FString& Remote)
	{
		return Remote.IsEmpty();
	});

	FString BranchName;
	FString BranchErrors;
	if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("symbolic-ref"), InPathToGitBinary, InRepositoryRoot,
		{ TEXT("--quiet"), TEXT("--short"), TEXT("HEAD") }, GetEmptyStringArray(), BranchName, BranchErrors, 0))
	{
		BranchName.TrimStartAndEndInline();
		if (!BranchName.IsEmpty())
		{
			FString UpstreamRemote;
			FString UpstreamErrors;
			if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), InPathToGitBinary, InRepositoryRoot,
				{ TEXT("--get"), FString::Printf(TEXT("branch.%s.remote"), *BranchName) }, GetEmptyStringArray(), UpstreamRemote, UpstreamErrors, 0))
			{
				UpstreamRemote.TrimStartAndEndInline();
				FString UpstreamMerge;
				FString UpstreamMergeErrors;
				FString VerifiedUpstream;
				FString VerifiedUpstreamErrors;
				const bool bHasConfiguredMerge = GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), InPathToGitBinary, InRepositoryRoot,
					{ TEXT("--get"), FString::Printf(TEXT("branch.%s.merge"), *BranchName) }, GetEmptyStringArray(), UpstreamMerge, UpstreamMergeErrors, 0, false);
				const bool bHasResolvedTrackingUpstream = GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot,
					{ TEXT("--verify"), TEXT("@{upstream}") }, GetEmptyStringArray(), VerifiedUpstream, VerifiedUpstreamErrors, 0, false);
				UpstreamMerge.TrimStartAndEndInline();
				if (bHasConfiguredMerge && !UpstreamMerge.IsEmpty() && bHasResolvedTrackingUpstream && FindConfiguredRemote(Remotes, UpstreamRemote, OutRemote))
				{
					return true;
				}
			}
		}
	}

	if (Remotes.Num() == 1)
	{
		OutRemote = Remotes[0];
		return true;
	}

	OutError = Remotes.IsEmpty()
		? TEXT("Git LFS content is missing locally and this repository has no remote. Configure a branch upstream or a single remote, then retry.")
		: TEXT("Git LFS content is missing locally and no remote could be selected. Configure the current branch upstream or leave exactly one remote, then retry.");
	return false;
}

bool ParseGitReleaseVersion(const FString& InVersionOutput, FGitReleaseVersion& OutVersion)
{
	OutVersion = FGitReleaseVersion();
	const FString Prefix = TEXT("git version ");
	if (!InVersionOutput.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}

	FString VersionToken = InVersionOutput.Mid(Prefix.Len());
	VersionToken.TrimStartAndEndInline();
	int32 FirstWhitespace = INDEX_NONE;
	for (int32 Index = 0; Index < VersionToken.Len(); ++Index)
	{
		if (FChar::IsWhitespace(VersionToken[Index]))
		{
			FirstWhitespace = Index;
			break;
		}
	}
	if (FirstWhitespace != INDEX_NONE)
	{
		VersionToken = VersionToken.Left(FirstWhitespace);
	}

	int32 Cursor = 0;
	auto ReadNumericComponent = [&VersionToken, &Cursor](int32& OutComponent) -> bool
	{
		const int32 Start = Cursor;
		int64 Value = 0;
		while (Cursor < VersionToken.Len() && FChar::IsDigit(VersionToken[Cursor]))
		{
			Value = Value * 10 + (VersionToken[Cursor] - TEXT('0'));
			if (Value > MAX_int32)
			{
				return false;
			}
			++Cursor;
		}
		if (Cursor == Start)
		{
			return false;
		}
		OutComponent = static_cast<int32>(Value);
		return true;
	};

	if (!ReadNumericComponent(OutVersion.Major) || Cursor >= VersionToken.Len() || VersionToken[Cursor++] != TEXT('.') ||
		!ReadNumericComponent(OutVersion.Minor) || Cursor >= VersionToken.Len() || VersionToken[Cursor++] != TEXT('.') ||
		!ReadNumericComponent(OutVersion.Patch))
	{
		return false;
	}

	// Git for Windows appends `.windows.N`; allow release packaging suffixes while
	// rejecting pre-release builds such as `2.53.0-rc1` and `2.53.0.rc1`.
	const FString Suffix = VersionToken.Mid(Cursor).ToLower();
	if (Suffix.StartsWith(TEXT("-"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (!Suffix.IsEmpty())
	{
		if (!Suffix.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			return false;
		}
		TArray<FString> SuffixSegments;
		Suffix.Mid(1).ParseIntoArray(SuffixSegments, TEXT("."), false);
		if (SuffixSegments.IsEmpty())
		{
			return false;
		}
		for (const FString& Segment : SuffixSegments)
		{
			if (Segment.IsEmpty() || Segment.StartsWith(TEXT("rc"), ESearchCase::CaseSensitive) ||
				Segment.StartsWith(TEXT("alpha"), ESearchCase::CaseSensitive) || Segment.StartsWith(TEXT("beta"), ESearchCase::CaseSensitive) ||
				Segment.StartsWith(TEXT("pre"), ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
	}
	return true;
}

bool IsSupportedGitRelease(const FGitReleaseVersion& InVersion)
{
	if (InVersion.Major != MinimumGitMajorVersion)
	{
		return InVersion.Major > MinimumGitMajorVersion;
	}
	if (InVersion.Minor != MinimumGitMinorVersion)
	{
		return InVersion.Minor > MinimumGitMinorVersion;
	}
	return InVersion.Patch >= MinimumGitPatchVersion;
}

bool ParseGitLfsReleaseVersion(const FString& InVersionOutput, FGitReleaseVersion& OutVersion)
{
	OutVersion = FGitReleaseVersion();
	const FString Prefix = TEXT("git-lfs/");
	if (!InVersionOutput.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	FString VersionToken = InVersionOutput.Mid(Prefix.Len());
	int32 FirstWhitespace = INDEX_NONE;
	for (int32 Index = 0; Index < VersionToken.Len(); ++Index)
	{
		if (FChar::IsWhitespace(VersionToken[Index]))
		{
			FirstWhitespace = Index;
			break;
		}
	}
	if (FirstWhitespace != INDEX_NONE)
	{
		VersionToken = VersionToken.Left(FirstWhitespace);
	}

	int32 Cursor = 0;
	auto ReadComponent = [&VersionToken, &Cursor](int32& OutComponent)
	{
		const int32 Start = Cursor;
		int64 Value = 0;
		while (Cursor < VersionToken.Len() && FChar::IsDigit(VersionToken[Cursor]))
		{
			Value = Value * 10 + (VersionToken[Cursor] - TEXT('0'));
			if (Value > MAX_int32)
			{
				return false;
			}
			++Cursor;
		}
		if (Cursor == Start)
		{
			return false;
		}
		OutComponent = static_cast<int32>(Value);
		return true;
	};
	return ReadComponent(OutVersion.Major) && Cursor < VersionToken.Len() && VersionToken[Cursor++] == TEXT('.') &&
		ReadComponent(OutVersion.Minor) && Cursor < VersionToken.Len() && VersionToken[Cursor++] == TEXT('.') &&
		ReadComponent(OutVersion.Patch) && Cursor == VersionToken.Len();
}

bool IsSupportedGitLfsRelease(const FGitReleaseVersion& InVersion)
{
	if (InVersion.Major != MinimumGitLfsMajorVersion)
	{
		return InVersion.Major > MinimumGitLfsMajorVersion;
	}
	if (InVersion.Minor != MinimumGitLfsMinorVersion)
	{
		return InVersion.Minor > MinimumGitLfsMinorVersion;
	}
	return InVersion.Patch >= MinimumGitLfsPatchVersion;
}

bool EnsureGitLfsCapability(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutError)
{
	OutError.Reset();
	const FString NormalizedPath = NormalizeGitBinaryPath(InPathToGitBinary);
	if (NormalizedPath.IsEmpty() || !FPaths::FileExists(NormalizedPath))
	{
		InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
		OutError = TEXT("The resolved Git executable is no longer available for Git LFS.");
		return false;
	}
	{
		FScopeLock Lock(&GitBinaryCapabilityCacheLock);
		if (GitLfsCapabilityCache.bIsSupported && GitLfsCapabilityCache.NormalizedPath == NormalizedPath)
		{
			return true;
		}
	}

	const FGitProcessResult Result = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, TEXT("lfs version"));
	if (Result.bCancelled || Result.bTimedOut)
	{
		OutError = Result.bCancelled
			? TEXT("Git LFS capability detection was cancelled.")
			: TEXT("Git LFS capability detection timed out.");
		return false;
	}
	FGitReleaseVersion Version;
	const bool bSupported = !Result.bLaunchFailed && Result.ReturnCode == 0 &&
		ParseGitLfsReleaseVersion(BytesToString(Result.StandardOutput), Version) && IsSupportedGitLfsRelease(Version);
	if (!bSupported)
	{
		if (Result.bLaunchFailed)
		{
			InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
		}
		OutError = BytesToString(Result.StandardError);
		OutError.TrimStartAndEndInline();
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Git LFS 3.7.1 or a newer release is required.");
		}
		return false;
	}
	{
		FScopeLock Lock(&GitBinaryCapabilityCacheLock);
		GitLfsCapabilityCache.NormalizedPath = NormalizedPath;
		GitLfsCapabilityCache.bIsSupported = true;
	}
	return true;
}

bool CheckGitAvailability(const FString& InPathToGitBinary)
{
	if (InPathToGitBinary.IsEmpty() || !FPaths::FileExists(InPathToGitBinary))
	{
		InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
		return false;
	}

	const FGitProcessResult Result = ExecuteGitProcessOffGameThread(InPathToGitBinary, FString(), TEXT("version"));
	FGitReleaseVersion Version;
	if (Result.bLaunchFailed || Result.bCancelled || Result.bTimedOut || Result.ReturnCode != 0 ||
		!ParseGitReleaseVersion(BytesToString(Result.StandardOutput), Version) || !IsSupportedGitRelease(Version))
	{
		InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
		UE_LOG(LogGitStandalone, Verbose, TEXT("Ignoring Git executable '%s': this plugin requires Git 2.53.0 or a newer release."), *InPathToGitBinary);
		return false;
	}

	CacheVerifiedGitBinary(InPathToGitBinary, Version);
	return true;
}
} // namespace GitSourceControlUtilsPrivate

using namespace GitSourceControlUtilsPrivate;

void FGitOperationCancellationContext::Cancel()
{
	bCancellationRequested.Store(true);
}

bool FGitOperationCancellationContext::IsCancellationRequested() const
{
	return bCancellationRequested.Load();
}

void InvalidateVerifiedGitBinary(const FString& InPathToGitBinary)
{
	InvalidateVerifiedGitBinaryInternal(InPathToGitBinary);
}

FGitOperationCancellationScope::FGitOperationCancellationScope(TSharedPtr<FGitOperationCancellationContext, ESPMode::ThreadSafe> InContext)
	: Context(MoveTemp(InContext))
	, PreviousContext(ActiveGitCancellationContext)
{
	if (Context.IsValid())
	{
		ActiveGitCancellationContext = Context;
	}
}

FGitOperationCancellationScope::~FGitOperationCancellationScope()
{
	if (Context.IsValid())
	{
		ActiveGitCancellationContext = MoveTemp(PreviousContext);
	}
}

// Launch a local Git command with a cancellable, timeout-bound child process.
bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode /* = 0 */, const bool bLogFailure /* = true */)
{
	OutResults.Reset();
	OutErrors.Reset();
	if (InPathToGitBinary.IsEmpty())
	{
		OutErrors = TEXT("Git binary path is empty.");
		return false;
	}

	FString Arguments = InCommand;
	for (const FString& Parameter : InParameters)
	{
		AppendGitArgument(Arguments, Parameter);
	}
	if (InFiles.Num() > 0)
	{
		Arguments += TEXT(" --");
		for (const FString& File : InFiles)
		{
			AppendGitArgument(Arguments, File);
		}
	}

	UE_LOG(LogGitStandalone, Verbose, TEXT("Run local Git command: git %s"), *InCommand);
	const FGitProcessResult ProcessResult = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, Arguments);
	OutResults = BytesToString(ProcessResult.StandardOutput);
	OutErrors = BytesToString(ProcessResult.StandardError);
	if (ProcessResult.bCancelled)
	{
		OutErrors = TEXT("Git command cancelled.");
		return false;
	}
	if (ProcessResult.bTimedOut)
	{
		OutErrors = FString::Printf(TEXT("Git command timed out after %.0f seconds."), GitCommandTimeoutSeconds);
		return false;
	}
	if (ProcessResult.ReturnCode != ExpectedReturnCode)
	{
		if (bLogFailure)
		{
			UE_LOG(LogGitStandalone, Warning, TEXT("Git command '%s' failed with exit code %d: %s"), *InCommand, ProcessResult.ReturnCode, *OutErrors);
		}
		return false;
	}

	return true;
}

// Basic parsing or results & errors from the Git command line process
static bool RunCommandInternal(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
							   const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternalRaw(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	Results.ParseIntoArray(OutResults, TEXT("\n"), true);
	Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

	return bResult;
}

FString FindGitBinaryPath()
{
	FString CachedGitBinary;
	if (TryGetVerifiedGitBinary(CachedGitBinary))
	{
		return CachedGitBinary;
	}

#if PLATFORM_WINDOWS
	FString GitBinaryPath;
	bool bFound = false;
	TSet<FString> TestedPaths;
	const FString PathEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	TArray<FString> PathEntries;
	PathEnvironment.ParseIntoArray(PathEntries, FPlatformMisc::GetPathVarDelimiter(), true);
	for (const FString& PathEntry : PathEntries)
	{
		const FString Candidate = FPaths::Combine(PathEntry, TEXT("git.exe"));
		FString CandidateKey = Candidate;
		FPaths::NormalizeFilename(CandidateKey);
		CandidateKey = CandidateKey.ToLower();
		if (!TestedPaths.Contains(CandidateKey) && FPaths::FileExists(Candidate) && CheckGitAvailability(Candidate))
		{
			GitBinaryPath = Candidate;
			bFound = true;
			break;
		}
		TestedPaths.Add(MoveTemp(CandidateKey));
	}

	// Fallback to conventional system locations when PATH has no usable Git.
	if (!bFound)
	{
		const TArray<FString> ProgramRoots
		{
			FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramW6432")),
			FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles")),
			FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles(x86)"))
		};
		for (const FString& ProgramRoot : ProgramRoots)
		{
			if (ProgramRoot.IsEmpty())
			{
				continue;
			}
			for (const TCHAR* GitSubpath : { TEXT("Git/bin/git.exe"), TEXT("Git/cmd/git.exe") })
			{
				GitBinaryPath = FPaths::Combine(ProgramRoot, GitSubpath);
				if (FPaths::FileExists(GitBinaryPath) && CheckGitAvailability(GitBinaryPath))
				{
					bFound = true;
					break;
				}
			}
			if (bFound)
			{
				break;
			}
		}
	}
	if (!bFound)
	{
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Programs/Git/cmd/git.exe"), *AppDataLocalPath);
		bFound = FPaths::FileExists(GitBinaryPath) && CheckGitAvailability(GitBinaryPath);
	}

	// 2) Else, look for the version of Git bundled with SmartGit "Installer with JRE"
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
		if (!bFound)
		{
			// If git is not found in "git/bin/" subdirectory, try the "bin/" path that was in use before
			GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/bin/git.exe");
			bFound = CheckGitAvailability(GitBinaryPath);
		}
	}

	// 3) Else, look for the local_git provided by SourceTree
	if (!bFound)
	{
		// C:\Users\UserName\AppData\Local\Atlassian\SourceTree\git_local\bin
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Atlassian/SourceTree/git_local/bin/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the PortableGit provided by GitHub Desktop
	if (!bFound)
	{
		// The latest GitHub Desktop adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\GitHub\PortableGit_c2ba306e536fdf878271f7fe636a147ff37326ad\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/GitHub/PortableGit_*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

	// 5) Else, look for the version of Git bundled with Tower
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/fournova/Tower/vendor/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 6) Else, look for the PortableGit provided by Fork
	if (!bFound)
	{
		// The latest Fork adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\Fork\gitInstance\2.39.1\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/Fork/gitInstance/*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/Fork/gitInstance/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/Fork/gitInstance/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#elif PLATFORM_MAC
	// 1) First of all, look for the version of git provided by official git
	FString GitBinaryPath = TEXT("/usr/local/git/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);

	// 2) Else, look for the version of git provided by Homebrew
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 2.1) else apple silicon brew stores git in different place
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/homebrew/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 3) Else, look for the version of git provided by MacPorts
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the version of git provided by Command Line Tools
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	{
		SCOPED_AUTORELEASE_POOL;
		NSWorkspace* SharedWorkspace = [NSWorkspace sharedWorkspace];

		// 5) Else, look for the version of local_git provided by SmartGit
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.syntevo.smartgit"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 6) Else, look for the version of local_git provided by SourceTree
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.torusknot.SourceTreeNotMAS"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git_local/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 7) Else, look for the version of local_git provided by GitHub Desktop
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.github.GitHubClient"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/app/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 8) Else, look for the version of local_git provided by Tower2
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.fournova.Tower2"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#else
	FString GitBinaryPath = TEXT("/usr/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);
#endif

	if (bFound)
	{
		FPaths::MakePlatformFilename(GitBinaryPath);
	}
	else
	{
		// If we did not find a path to Git, set it empty
		GitBinaryPath.Empty();
	}

	return GitBinaryPath;
}

bool ResolveStandaloneRepositoryForFile(const FString& InFilename, FString& OutGitBinary, FString& OutRepositoryRoot, FString& OutError)
{
	OutGitBinary.Reset();
	OutRepositoryRoot.Reset();
	OutError.Reset();
	FString Filename = FPaths::ConvertRelativePathToFull(InFilename);
	FPaths::NormalizeFilename(Filename);
	if (Filename.IsEmpty())
	{
		OutError = TEXT("A workspace filename is required.");
		return false;
	}

	OutGitBinary = FindGitBinaryPath();
	if (OutGitBinary.IsEmpty())
	{
		OutError = TEXT("Could not locate a supported local Git executable. Git 2.53.0 or a newer release is required.");
		return false;
	}
	if (!FindRootDirectory(Filename, OutRepositoryRoot))
	{
		OutError = FString::Printf(TEXT("The file is not inside a Git repository: %s"), *Filename);
		OutRepositoryRoot.Reset();
		return false;
	}
	OutRepositoryRoot = FPaths::ConvertRelativePathToFull(OutRepositoryRoot);
	FPaths::NormalizeDirectoryName(OutRepositoryRoot);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace Testing
{
	void ResetGitProcessLaunchCount()
	{
		GitSourceControlUtilsPrivate::GitProcessLaunchCount.Store(0);
		GitSourceControlUtilsPrivate::GitLfsFetchLaunchCount.Store(0);
	}

void ResetVerifiedGitBinaryCache()
{
	FScopeLock Lock(&GitSourceControlUtilsPrivate::GitBinaryCapabilityCacheLock);
	GitSourceControlUtilsPrivate::GitBinaryCapabilityCache = GitSourceControlUtilsPrivate::FGitBinaryCapabilityCache();
	GitSourceControlUtilsPrivate::GitLfsCapabilityCache = GitSourceControlUtilsPrivate::FGitLfsCapabilityCache();
}

	uint64 GetGitProcessLaunchCount()
	{
		return GitSourceControlUtilsPrivate::GitProcessLaunchCount.Load();
	}

	uint64 GetGitLfsFetchLaunchCount()
	{
		return GitSourceControlUtilsPrivate::GitLfsFetchLaunchCount.Load();
	}

	uint64 GetGitProcessLaunchCountAtModuleStartup()
	{
		return GitSourceControlUtilsPrivate::GitProcessLaunchCountAtModuleStartup.Load();
	}

	void CaptureGitProcessLaunchCountAtModuleStartup()
	{
		GitSourceControlUtilsPrivate::GitProcessLaunchCountAtModuleStartup.Store(GitSourceControlUtilsPrivate::GitProcessLaunchCount.Load());
	}

	bool LoadStandaloneHistory(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InFilename,
		const EGitLocalSourceControlHistoryMode InMode, FString& OutCapturedHead, bool& bOutHeadChanged,
		TArray<FGitStandaloneHistoryTestEntry>& OutHistory, FString& OutError)
	{
		OutHistory.Reset();
		OutError.Reset();
		TArray<FString> Errors;
		TGitSourceControlHistory History;
		if (!RunGetHistory(InGitBinary, InRepositoryRoot, InFilename, false, InMode, OutCapturedHead, bOutHeadChanged, Errors, History))
		{
			OutError = FString::Join(Errors, TEXT("\n"));
			return false;
		}
		for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : History)
		{
			FGitStandaloneHistoryTestEntry& Entry = OutHistory.AddDefaulted_GetRef();
			Entry.CommitId = Revision->CommitId;
			Entry.HistoricalPath = Revision->Filename;
			Entry.LocalFilename = Revision->LocalFilename;
			Entry.Description = Revision->Description;
			Entry.Author = Revision->UserName;
			Entry.Action = Revision->Action;
		}
		return true;
	}

	TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> MakeStandaloneRevision(const FString& InGitBinary, const FString& InRepositoryRoot,
		const FString& InLocalFilename, const FString& InCommitId, const FString& InHistoricalPath)
	{
		TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FGitSourceControlRevision, ESPMode::ThreadSafe>();
		Revision->GitBinary = InGitBinary;
		Revision->RepositoryRoot = InRepositoryRoot;
		Revision->LocalFilename = InLocalFilename;
		Revision->CommitId = InCommitId;
		Revision->Filename = InHistoricalPath;
		return Revision;
	}

	bool ExportStandaloneRevisionForDiff(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InLocalFilename,
		const FString& InCommitId, const FString& InHistoricalPath, FString& OutTempFilename)
	{
		const TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeStandaloneRevision(
			InGitBinary, InRepositoryRoot, InLocalFilename, InCommitId, InHistoricalPath);
		OutTempFilename.Reset();
		return Revision->Get(OutTempFilename);
	}

	UPackage* LoadStandaloneRevisionPackageForDiff(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InLocalFilename,
		const FString& InCommitId, const FString& InHistoricalPath)
	{
		const TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeStandaloneRevision(
			InGitBinary, InRepositoryRoot, InLocalFilename, InCommitId, InHistoricalPath);
		const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> SourceRevision = StaticCastSharedPtr<ISourceControlRevision>(Revision);
		return DiffUtils::LoadPackageForDiff(SourceRevision);
	}
}
#endif

bool VerifyLocalLfsObject(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InObjectFilename, const FString& InExpectedOid, const int64 InExpectedSize, FString& OutError)
{
	OutError.Reset();
	if (InPathToGitBinary.IsEmpty() || InObjectFilename.IsEmpty() || InExpectedOid.Len() != 64 || InExpectedSize < 0)
	{
		OutError = TEXT("Invalid Git LFS object verification arguments.");
		return false;
	}
	if (!IFileManager::Get().FileExists(*InObjectFilename))
	{
		OutError = TEXT("The local Git LFS object is missing. Fetch it with an external Git client and retry.");
		return false;
	}
	const int64 ObjectSize = IFileManager::Get().FileSize(*InObjectFilename);
	if (ObjectSize != InExpectedSize)
	{
		OutError = FString::Printf(TEXT("The local Git LFS object has size %lld, but the pointer requires %lld bytes."), ObjectSize, InExpectedSize);
		return false;
	}
	if (!EnsureGitLfsCapability(InPathToGitBinary, InRepositoryRoot, OutError))
	{
		return false;
	}

	FString Arguments = TEXT("lfs");
	AppendGitArgument(Arguments, TEXT("pointer"));
	AppendGitArgument(Arguments, TEXT("--file=") + InObjectFilename);
	const FGitProcessResult ProcessResult = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, Arguments);
	if (ProcessResult.bCancelled)
	{
		OutError = TEXT("Git LFS object verification was cancelled.");
		return false;
	}
	if (ProcessResult.bTimedOut)
	{
		OutError = FString::Printf(TEXT("Git LFS object verification timed out after %.0f seconds."), GitCommandTimeoutSeconds);
		return false;
	}
	if (ProcessResult.ReturnCode != 0)
	{
		OutError = BytesToString(ProcessResult.StandardError);
		OutError.TrimStartAndEndInline();
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The resolved Git binary could not run local Git LFS pointer verification. Install Git LFS and retry.");
		}
		return false;
	}

	FString ActualOid;
	int64 ActualSize = 0;
	if (!ParseLfsPointerOutput(BytesToString(ProcessResult.StandardOutput), ActualOid, ActualSize) ||
		!ActualOid.Equals(InExpectedOid, ESearchCase::IgnoreCase) || ActualSize != InExpectedSize)
	{
		OutError = FString::Printf(TEXT("The local Git LFS object failed SHA-256 verification for %s."), *InExpectedOid);
		return false;
	}
	return true;
}

bool FetchLfsContentForRevision(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
	const FString& InFullCommitId, const FString& InHistoricalPath, FString& OutError)
{
	OutError.Reset();
	FString HistoricalPath;
	if (!NormalizeHistoricalLfsPath(InHistoricalPath, HistoricalPath))
	{
		OutError = TEXT("Git LFS historical fetch requires one strict literal repository path; patterns, lists, traversal, backslashes and control characters are rejected.");
		return false;
	}
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || !IsFullGitCommitId(InFullCommitId))
	{
		OutError = TEXT("Git binary, repository root, complete commit id and a repository-relative historical path are required for Git LFS download.");
		return false;
	}
	if (!EnsureGitLfsCapability(InPathToGitBinary, InRepositoryRoot, OutError))
	{
		return false;
	}

	FString Remote;
	if (!ResolveLfsFetchRemote(InPathToGitBinary, InRepositoryRoot, Remote, OutError))
	{
		return false;
	}

	FString Arguments = TEXT("lfs");
	AppendGitArgument(Arguments, TEXT("fetch"));
	AppendGitArgument(Arguments, Remote);
	AppendGitArgument(Arguments, InFullCommitId);
	AppendGitArgument(Arguments, TEXT("--include=") + HistoricalPath);
	AppendGitArgument(Arguments, TEXT("--exclude="));
	UE_LOG(LogGitStandalone, Log, TEXT("Fetching requested Git LFS revision content from remote '%s'."), *Remote);
#if WITH_DEV_AUTOMATION_TESTS
	++GitSourceControlUtilsPrivate::GitLfsFetchLaunchCount;
#endif
	const FGitProcessResult ProcessResult = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, Arguments, GitNetworkCommandTimeoutSeconds);
	if (ProcessResult.bCancelled)
	{
		OutError = TEXT("Git LFS download was cancelled.");
		return false;
	}
	if (ProcessResult.bTimedOut)
	{
		OutError = FString::Printf(TEXT("Git LFS download timed out after %.0f seconds."), GitNetworkCommandTimeoutSeconds);
		return false;
	}
	if (ProcessResult.ReturnCode != 0)
	{
		OutError = BytesToString(ProcessResult.StandardError);
		OutError.TrimStartAndEndInline();
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Git LFS download from remote '%s' failed."), *Remote);
		}
		return false;
	}
	return true;
}

// Find the root of the Git repository, looking from the provided path and upward in its parent directories.
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot)
{
	OutRepositoryRoot = InPath;

	auto TrimTrailing = [](FString& Str, const TCHAR Char) {
		int32 Len = Str.Len();
		while (Len && Str[Len - 1] == Char)
		{
			Str = Str.LeftChop(1);
			Len = Str.Len();
		}
	};

	TrimTrailing(OutRepositoryRoot, '\\');
	TrimTrailing(OutRepositoryRoot, '/');

	bool bFound = false;
	FString PathToGitSubdirectory;
	while (!bFound && !OutRepositoryRoot.IsEmpty())
	{
		// Look for the ".git" subdirectory (or file) present at the root of every Git repository
		PathToGitSubdirectory = OutRepositoryRoot / TEXT(".git");
		bFound = IFileManager::Get().DirectoryExists(*PathToGitSubdirectory) || IFileManager::Get().FileExists(*PathToGitSubdirectory);
		if (!bFound)
		{
			int32 LastSlashIndex;
			if (OutRepositoryRoot.FindLastChar('/', LastSlashIndex))
			{
				OutRepositoryRoot = OutRepositoryRoot.Left(LastSlashIndex);
			}
			else
			{
				OutRepositoryRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutRepositoryRoot = InPath; // If not found, return the provided dir as best possible root.
	}
	return bFound;
}

void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("user.name"));
	bResults = RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserName = InfoMessages[0];
	}
	else
	{
		OutUserName = TEXT("");
	}

	Parameters.Reset(1);
	Parameters.Add(TEXT("user.email"));
	InfoMessages.Reset();
	bResults &= RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserEmail = InfoMessages[0];
	}
	else
	{
		OutUserEmail = TEXT("");
	}
}

bool RunCommand(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
				const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	return RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
}

TArray<UPackage*> UnlinkPackages(const TArray<FString>& InPackageNames)
{
	TArray<UPackage*> LoadedPackages;
	// UE-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	if (InPackageNames.Num() > 0)
	{
		TArray<FString> PackagesToUnlink;
		for (const auto& Filename : InPackageNames)
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				PackagesToUnlink.Add(*PackageName);
			}
		}
		// Form a list of loaded packages to reload...
		LoadedPackages.Reserve(PackagesToUnlink.Num());
		for (const FString& PackageName : PackagesToUnlink)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (Package)
			{
				LoadedPackages.Emplace(Package);

				// Detach the linkers of any loaded packages so that SCC can overwrite the files...
				if (!Package->IsFullyLoaded())
				{
					FlushAsyncLoading();
					Package->FullyLoad();
				}
				ResetLoaders(Package);
			}
		}
	}
	return LoadedPackages;
}

void ReloadPackages(TArray<UPackage*>& InPackagesToReload)
{
	// UE-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InPackagesToReload.RemoveAll([&](UPackage* InPackage) -> bool {
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	UPackageTools::ReloadPackages(InPackagesToReload);

	// Unload any deleted packages...
	UPackageTools::UnloadPackages(PackagesToUnload);
}

/// Convert filename relative to the repository root to absolute path (inplace)
void AbsoluteFilenames(const FString& InRepositoryRoot, TArray<FString>& InFileNames)
{
	for (auto& FileName : InFileNames)
	{
		FileName = FPaths::ConvertRelativePathToFull(InRepositoryRoot, FileName);
	}
}

namespace GitSourceControlStatusPrivate
{
bool RunLocalCommand(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InArguments, TArray<uint8>& OutStandardOutput, TArray<FString>& OutErrorMessages)
{
	const FGitProcessResult ProcessResult = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, InArguments);
	OutStandardOutput = ProcessResult.StandardOutput;
	const FString StandardError = BytesToString(ProcessResult.StandardError);
	if (ProcessResult.bCancelled)
	{
		OutErrorMessages.Add(TEXT("Git status request cancelled."));
		return false;
	}
	if (ProcessResult.bTimedOut)
	{
		OutErrorMessages.Add(FString::Printf(TEXT("Git status request timed out after %.0f seconds."), GitCommandTimeoutSeconds));
		return false;
	}
	if (ProcessResult.ReturnCode != 0)
	{
		OutErrorMessages.Add(StandardError.IsEmpty() ? TEXT("Git status request failed.") : StandardError);
		return false;
	}
	return true;
}

bool CheckLocalGitCapabilitiesInternal(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutError)
{
	OutError.Reset();
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty())
	{
		OutError = TEXT("Git binary path and repository root are required.");
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TArray<FString> Errors;
	TArray<uint8> StatusOutput;
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot,
		TEXT("--no-optional-locks --literal-pathspecs status --porcelain=v2 -z --untracked-files=no -- .git"), StatusOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Git does not support porcelain v2 NUL status output.") : Errors[0];
		return false;
	}

	TArray<uint8> LsFilesOutput;
	Errors.Reset();
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot, TEXT("--no-optional-locks --literal-pathspecs ls-files -z -- .git"), LsFilesOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Git does not support literal path-scoped file queries.") : Errors[0];
		return false;
	}
	return true;
}

bool RunStatusCommands(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, TArray<uint8>& OutStandardOutput, TArray<FString>& OutErrorMessages)
{
	// Keep all paths literal and split only when an OS command line would be too long.
	constexpr int32 MaxCommandLineLength = 24000;
	const FString Prefix = TEXT("--no-optional-locks --literal-pathspecs status --porcelain=v2 -z --renames --untracked-files=all --ignored=traditional --");
	FString Arguments = Prefix;
	for (const FString& File : InFiles)
	{
		FString RelativeFile = File;
		if (!MakeRepositoryRelativePath(InRepositoryRoot, RelativeFile))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Path is outside the Git repository: %s"), *File));
			return false;
		}
		FPaths::NormalizeFilename(RelativeFile);
		const FString QuotedPath = QuoteGitArgument(RelativeFile);
		if (Arguments.Len() + QuotedPath.Len() + 1 > MaxCommandLineLength && Arguments.Len() > Prefix.Len())
		{
			TArray<uint8> BatchOutput;
			if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, Arguments, BatchOutput, OutErrorMessages))
			{
				return false;
			}
			OutStandardOutput.Append(BatchOutput);
			Arguments = Prefix;
		}
		Arguments += TEXT(" ");
		Arguments += QuotedPath;
	}

	TArray<uint8> BatchOutput;
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, Arguments, BatchOutput, OutErrorMessages))
	{
		return false;
	}
	OutStandardOutput.Append(BatchOutput);
	return true;
}

bool RunLsFilesCommands(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, const FString& InOptions, TArray<uint8>& OutStandardOutput, TArray<FString>& OutErrorMessages)
{
	constexpr int32 MaxCommandLineLength = 24000;
	const FString Prefix = TEXT("--no-optional-locks --literal-pathspecs ls-files ") + InOptions + TEXT(" -z --");
	FString Arguments = Prefix;
	for (const FString& File : InFiles)
	{
		FString RelativeFile = File;
		if (!MakeRepositoryRelativePath(InRepositoryRoot, RelativeFile))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Path is outside the Git repository: %s"), *File));
			return false;
		}
		FPaths::NormalizeFilename(RelativeFile);
		const FString QuotedPath = QuoteGitArgument(RelativeFile);
		if (Arguments.Len() + QuotedPath.Len() + 1 > MaxCommandLineLength && Arguments.Len() > Prefix.Len())
		{
			TArray<uint8> BatchOutput;
			if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, Arguments, BatchOutput, OutErrorMessages))
			{
				return false;
			}
			OutStandardOutput.Append(BatchOutput);
			Arguments = Prefix;
		}
		Arguments += TEXT(" ");
		Arguments += QuotedPath;
	}

	TArray<uint8> BatchOutput;
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, Arguments, BatchOutput, OutErrorMessages))
	{
		return false;
	}
	OutStandardOutput.Append(BatchOutput);
	return true;
}

bool BuildLiteralPathArguments(const FString& InRepositoryRoot, const FString& InVerb, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutArguments, FString& OutError)
{
	constexpr int32 MaxCommandLineLength = 24000;
	OutArguments = TEXT("--no-optional-locks --literal-pathspecs ") + InVerb;
	for (const FString& Parameter : InParameters)
	{
		AppendGitArgument(OutArguments, Parameter);
	}
	OutArguments += TEXT(" --");
	for (const FString& File : InFiles)
	{
		FString RelativeFile = File;
		if (!MakeRepositoryRelativePath(InRepositoryRoot, RelativeFile))
		{
			OutError = FString::Printf(TEXT("Path is outside the Git repository: %s"), *File);
			return false;
		}
		FPaths::NormalizeFilename(RelativeFile);
		const FString QuotedPath = QuoteGitArgument(RelativeFile);
		if (OutArguments.Len() + QuotedPath.Len() + 1 > MaxCommandLineLength)
		{
			OutError = TEXT("The selected file set exceeds Git's safe command-line limit.");
			return false;
		}
		OutArguments += TEXT(" ");
		OutArguments += QuotedPath;
	}
	return true;
}

FString NormalizeFileKey(const FString& InFilename)
{
	FString Result = FPaths::ConvertRelativePathToFull(InFilename);
	FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
	Result = Result.ToLower();
#endif
	return Result;
}

FString MakeAbsoluteStatusPath(const FString& InRepositoryRoot, const FString& InRelativeFilename)
{
	FString Result = FPaths::ConvertRelativePathToFull(InRepositoryRoot, InRelativeFilename);
	FPaths::NormalizeFilename(Result);
	return Result;
}

bool ReadNulToken(const TArray<uint8>& InData, int32& InOutOffset, FString& OutToken)
{
	if (InOutOffset >= InData.Num())
	{
		return false;
	}

	const int32 Start = InOutOffset;
	while (InOutOffset < InData.Num() && InData[InOutOffset] != 0)
	{
		++InOutOffset;
	}
	FFileHelper::BufferToString(OutToken, InData.GetData() + Start, InOutOffset - Start);
	if (InOutOffset < InData.Num())
	{
		++InOutOffset;
	}
	return true;
}

bool ExtractPorcelainV2Path(const FString& InRecord, const int32 InFieldsBeforePath, FString& OutPath)
{
	int32 SpacesSeen = 0;
	for (int32 Index = 0; Index < InRecord.Len(); ++Index)
	{
		if (InRecord[Index] == TEXT(' '))
		{
			++SpacesSeen;
			if (SpacesSeen == InFieldsBeforePath)
			{
				OutPath = InRecord.Mid(Index + 1);
				return !OutPath.IsEmpty();
			}
		}
	}
	return false;
}

void ApplyPorcelainV2State(const TCHAR InRecordType, const FString& InXY, FGitSourceControlFileStatus& OutState)
{
	if (InRecordType == TEXT('?'))
	{
		OutState.FileState = EGitFileState::Unknown;
		OutState.TreeState = EGitTreeState::Untracked;
		return;
	}
	if (InRecordType == TEXT('!'))
	{
		OutState.FileState = EGitFileState::Unknown;
		OutState.TreeState = EGitTreeState::Ignored;
		return;
	}
	if (InRecordType == TEXT('u') || InXY.Len() != 2)
	{
		OutState.FileState = EGitFileState::Unmerged;
		OutState.TreeState = EGitTreeState::Working;
		return;
	}

	const TCHAR IndexState = InXY[0];
	const TCHAR WorktreeState = InXY[1];
	if (IndexState == TEXT('U') || WorktreeState == TEXT('U') || (IndexState == TEXT('A') && WorktreeState == TEXT('A')) || (IndexState == TEXT('D') && WorktreeState == TEXT('D')))
	{
		OutState.FileState = EGitFileState::Unmerged;
		OutState.TreeState = EGitTreeState::Working;
		return;
	}

	OutState.TreeState = IndexState == TEXT('.') ? EGitTreeState::Working : (WorktreeState == TEXT('.') ? EGitTreeState::Staged : EGitTreeState::Working);
	if (IndexState == TEXT('A'))
	{
		OutState.FileState = EGitFileState::Added;
	}
	else if (IndexState == TEXT('D'))
	{
		OutState.FileState = EGitFileState::Deleted;
	}
	else if (WorktreeState == TEXT('D'))
	{
		OutState.FileState = EGitFileState::Deleted;
	}
	else if (IndexState == TEXT('R'))
	{
		OutState.FileState = EGitFileState::Renamed;
	}
	else if (IndexState == TEXT('C'))
	{
		OutState.FileState = EGitFileState::Copied;
	}
	else
	{
		OutState.FileState = EGitFileState::Modified;
	}
}

void AddPorcelainV2State(const FString& InRepositoryRoot, const FString& InRelativePath, const TCHAR InRecordType, const FString& InXY, TMap<FString, FGitSourceControlFileStatus>& OutStates)
{
	if (InRelativePath.IsEmpty())
	{
		return;
	}
	const FString AbsolutePath = MakeAbsoluteStatusPath(InRepositoryRoot, InRelativePath);
	FGitSourceControlFileStatus State;
	ApplyPorcelainV2State(InRecordType, InXY, State);
	OutStates.Add(NormalizeFileKey(AbsolutePath), MoveTemp(State));
}

void ParsePorcelainV2Status(const TArray<uint8>& InOutput, const FString& InRepositoryRoot, TMap<FString, FGitSourceControlFileStatus>& OutStates)
{
	int32 Offset = 0;
	FString Record;
	while (ReadNulToken(InOutput, Offset, Record))
	{
		if (Record.Len() < 2)
		{
			continue;
		}

		const TCHAR RecordType = Record[0];
		if (RecordType == TEXT('?') || RecordType == TEXT('!'))
		{
			AddPorcelainV2State(InRepositoryRoot, Record.Mid(2), RecordType, FString(), OutStates);
			continue;
		}

		int32 FieldsBeforePath = 0;
		if (RecordType == TEXT('1'))
		{
			FieldsBeforePath = 8;
		}
		else if (RecordType == TEXT('2'))
		{
			FieldsBeforePath = 9;
		}
		else if (RecordType == TEXT('u'))
		{
			FieldsBeforePath = 10;
		}
		else
		{
			continue;
		}

		FString RelativePath;
		if (!ExtractPorcelainV2Path(Record, FieldsBeforePath, RelativePath))
		{
			continue;
		}
		const FString XY = Record.Len() >= 4 ? Record.Mid(2, 2) : FString();
		AddPorcelainV2State(InRepositoryRoot, RelativePath, RecordType, XY, OutStates);
		if (RecordType == TEXT('2'))
		{
			FString OriginalPath;
			if (ReadNulToken(InOutput, Offset, OriginalPath))
			{
				AddPorcelainV2State(InRepositoryRoot, OriginalPath, RecordType, XY, OutStates);
			}
		}
	}
}

void ParseTrackedPaths(const TArray<uint8>& InOutput, const FString& InRepositoryRoot, TSet<FString>& OutTrackedPaths)
{
	int32 Offset = 0;
	FString RelativePath;
	while (ReadNulToken(InOutput, Offset, RelativePath))
	{
		if (!RelativePath.IsEmpty())
		{
			OutTrackedPaths.Add(NormalizeFileKey(MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath)));
		}
	}
}

bool ReadGitObjectId(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InArguments, FString& OutObjectId, TArray<FString>& OutErrorMessages)
{
	TArray<uint8> Output;
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, InArguments, Output, OutErrorMessages))
	{
		return false;
	}
	FFileHelper::BufferToString(OutObjectId, Output.GetData(), Output.Num());
	OutObjectId.TrimStartAndEndInline();
	if (OutObjectId.IsEmpty())
	{
		OutErrorMessages.Add(TEXT("Git did not return a content object id while matching an unstaged rename."));
		return false;
	}
	return true;
}

bool AppendUnstagedRenameFallbackPairs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TSet<FString>& InSelectedKeys,
	TSet<FString>& InOutExpandedKeys, TArray<FString>& InOutExpandedFiles, TArray<FGitRenamePair>& InOutRenamePairs, TArray<FString>& OutErrorMessages)
{
	TArray<uint8> StatusOutput;
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot,
		TEXT("--no-optional-locks --literal-pathspecs status --porcelain=v2 -z --renames --untracked-files=all --ignored=no"), StatusOutput, OutErrorMessages))
	{
		return false;
	}

	TArray<FString> DeletedPaths;
	TArray<FString> UntrackedPaths;
	TSet<FString> DeletedKeys;
	TSet<FString> UntrackedKeys;
	int32 Offset = 0;
	FString Record;
	while (ReadNulToken(StatusOutput, Offset, Record))
	{
		if (Record.StartsWith(TEXT("? ")))
		{
			const FString RelativePath = Record.Mid(2);
			const FString AbsolutePath = MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath);
			const FString Key = NormalizeFileKey(AbsolutePath);
			if (!RelativePath.IsEmpty() && !FPaths::DirectoryExists(AbsolutePath) && !UntrackedKeys.Contains(Key))
			{
				UntrackedKeys.Add(Key);
				UntrackedPaths.Add(RelativePath);
			}
			continue;
		}
		if (!Record.StartsWith(TEXT("1 ")) || Record.Len() < 4 || (Record[2] != TEXT('D') && Record[3] != TEXT('D')))
		{
			continue;
		}
		FString RelativePath;
		if (!ExtractPorcelainV2Path(Record, 8, RelativePath) || RelativePath.IsEmpty())
		{
			OutErrorMessages.Add(TEXT("Malformed Git deletion record while matching an unstaged rename."));
			return false;
		}
		const FString AbsolutePath = MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath);
		const FString Key = NormalizeFileKey(AbsolutePath);
		if (!DeletedKeys.Contains(Key))
		{
			DeletedKeys.Add(Key);
			DeletedPaths.Add(RelativePath);
		}
	}

	bool bSelectedDeletedPath = false;
	for (const FString& RelativePath : DeletedPaths)
	{
		bSelectedDeletedPath |= InSelectedKeys.Contains(NormalizeFileKey(MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath)));
	}
	bool bSelectedUntrackedPath = false;
	for (const FString& RelativePath : UntrackedPaths)
	{
		bSelectedUntrackedPath |= InSelectedKeys.Contains(NormalizeFileKey(MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath)));
	}
	if (!bSelectedDeletedPath && !bSelectedUntrackedPath)
	{
		return true;
	}

	TMap<FString, TArray<FString>> DeletedPathsByObjectId;
	for (const FString& RelativePath : DeletedPaths)
	{
		FString ObjectId;
		const FString Arguments = FString::Printf(TEXT("--no-optional-locks rev-parse --verify HEAD:%s"), *QuoteGitArgument(RelativePath));
		if (!ReadGitObjectId(InPathToGitBinary, InRepositoryRoot, Arguments, ObjectId, OutErrorMessages))
		{
			return false;
		}
		DeletedPathsByObjectId.FindOrAdd(ObjectId).Add(RelativePath);
	}
	TMap<FString, TArray<FString>> UntrackedPathsByObjectId;
	for (const FString& RelativePath : UntrackedPaths)
	{
		FString ObjectId;
		const FString Arguments = FString::Printf(TEXT("--no-optional-locks --literal-pathspecs hash-object --path=%s -- %s"), *QuoteGitArgument(RelativePath), *QuoteGitArgument(RelativePath));
		if (!ReadGitObjectId(InPathToGitBinary, InRepositoryRoot, Arguments, ObjectId, OutErrorMessages))
		{
			return false;
		}
		UntrackedPathsByObjectId.FindOrAdd(ObjectId).Add(RelativePath);
	}

	auto AppendPair = [&InOutExpandedKeys, &InOutExpandedFiles, &InOutRenamePairs, &InRepositoryRoot](const FString& OldRelativePath, const FString& NewRelativePath) -> bool
	{
		FGitRenamePair Pair;
		Pair.OldPath = MakeAbsoluteStatusPath(InRepositoryRoot, OldRelativePath);
		Pair.NewPath = MakeAbsoluteStatusPath(InRepositoryRoot, NewRelativePath);
		const FString OldKey = NormalizeFileKey(Pair.OldPath);
		const FString NewKey = NormalizeFileKey(Pair.NewPath);
		if (InOutExpandedKeys.Contains(OldKey) && InOutExpandedKeys.Contains(NewKey))
		{
			return true;
		}
		if (!InOutExpandedKeys.Contains(OldKey))
		{
			InOutExpandedKeys.Add(OldKey);
			InOutExpandedFiles.Add(Pair.OldPath);
		}
		if (!InOutExpandedKeys.Contains(NewKey))
		{
			InOutExpandedKeys.Add(NewKey);
			InOutExpandedFiles.Add(Pair.NewPath);
		}
		InOutRenamePairs.Add(MoveTemp(Pair));
		return true;
	};

	for (const FString& RelativePath : DeletedPaths)
	{
		const FString AbsolutePath = MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath);
		if (!InSelectedKeys.Contains(NormalizeFileKey(AbsolutePath)))
		{
			continue;
		}
		const FString* ObjectId = nullptr;
		for (const TPair<FString, TArray<FString>>& Pair : DeletedPathsByObjectId)
		{
			if (Pair.Value.Contains(RelativePath))
			{
				ObjectId = &Pair.Key;
				break;
			}
		}
		if (!ObjectId)
		{
			continue;
		}
		const TArray<FString>* Matches = UntrackedPathsByObjectId.Find(*ObjectId);
		if (!Matches || Matches->IsEmpty())
		{
			continue;
		}
		if (Matches->Num() != 1)
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Unstaged rename from '%s' has %d identical untracked destination candidates; use an external Git GUI."), *RelativePath, Matches->Num()));
			return false;
		}
		if (!AppendPair(RelativePath, (*Matches)[0]))
		{
			return false;
		}
	}

	for (const FString& RelativePath : UntrackedPaths)
	{
		const FString AbsolutePath = MakeAbsoluteStatusPath(InRepositoryRoot, RelativePath);
		if (!InSelectedKeys.Contains(NormalizeFileKey(AbsolutePath)))
		{
			continue;
		}
		const FString* ObjectId = nullptr;
		for (const TPair<FString, TArray<FString>>& Pair : UntrackedPathsByObjectId)
		{
			if (Pair.Value.Contains(RelativePath))
			{
				ObjectId = &Pair.Key;
				break;
			}
		}
		if (!ObjectId)
		{
			continue;
		}
		const TArray<FString>* Matches = DeletedPathsByObjectId.Find(*ObjectId);
		if (!Matches || Matches->IsEmpty())
		{
			continue;
		}
		if (Matches->Num() != 1)
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Unstaged rename to '%s' has %d identical deleted source candidates; use an external Git GUI."), *RelativePath, Matches->Num()));
			return false;
		}
		if (!AppendPair((*Matches)[0], RelativePath))
		{
			return false;
		}
	}
	return true;
}
} // namespace GitSourceControlStatusPrivate

using namespace GitSourceControlStatusPrivate;

bool CheckLocalGitCapabilities(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutError)
{
	return CheckLocalGitCapabilitiesInternal(InPathToGitBinary, InRepositoryRoot, OutError);
}

bool RunRepositoryStatusPorcelainV2(const FString& InPathToGitBinary, const FString& InRepositoryRoot,
	TArray<uint8>& OutStandardOutput, FString& OutError)
{
	OutStandardOutput.Reset();
	OutError.Reset();
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty())
	{
		OutError = TEXT("Git binary path and repository root are required.");
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TArray<FString> Errors;
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot,
		TEXT("--no-optional-locks --literal-pathspecs status --porcelain=v2 --branch --no-ahead-behind -z --renames --untracked-files=all --ignored=no"),
		OutStandardOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Repository-wide Git status query failed.") : Errors[0];
		return false;
	}
	return true;
}

bool RunPathsStatusPorcelainV2(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles,
	TArray<uint8>& OutStandardOutput, FString& OutError)
{
	OutStandardOutput.Reset();
	OutError.Reset();
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || InFiles.IsEmpty())
	{
		OutError = TEXT("Git binary path, repository root, and one or more exact .uasset paths are required.");
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	constexpr int32 MaxCommandLineLength = 24000;
	FString Arguments = TEXT("--no-optional-locks --literal-pathspecs status --porcelain=v2 -z --renames --untracked-files=all --ignored=no --");
	TSet<FString> SeenRelativePaths;
	for (const FString& InFile : InFiles)
	{
		FString AbsoluteFilename = FPaths::ConvertRelativePathToFull(InFile);
		FPaths::NormalizeFilename(AbsoluteFilename);
		if (FPaths::DirectoryExists(AbsoluteFilename) || !FPaths::GetExtension(AbsoluteFilename, false).Equals(TEXT("uasset"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Changed Assets status only accepts explicit .uasset files: %s"), *InFile);
			return false;
		}

		FString RelativeFilename = AbsoluteFilename;
		if (!MakeRepositoryRelativePath(RepositoryRoot, RelativeFilename))
		{
			OutError = FString::Printf(TEXT("Changed Assets status path is outside the Git repository: %s"), *InFile);
			return false;
		}
		FPaths::NormalizeFilename(RelativeFilename);
		const FString PathKey = NormalizeFileKey(AbsoluteFilename);
		if (SeenRelativePaths.Contains(PathKey))
		{
			continue;
		}

		const FString QuotedPath = QuoteGitArgument(RelativeFilename);
		if (Arguments.Len() + QuotedPath.Len() + 1 > MaxCommandLineLength)
		{
			OutError = TEXT("The exact Changed Assets recheck path set exceeds the one-process command-line limit.");
			return false;
		}
		SeenRelativePaths.Add(PathKey);
		Arguments += TEXT(" ");
		Arguments += QuotedPath;
	}
	if (SeenRelativePaths.IsEmpty())
	{
		OutError = TEXT("Changed Assets status requires at least one unique .uasset path.");
		return false;
	}

	TArray<FString> Errors;
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot, Arguments, OutStandardOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Exact Changed Assets status query failed.") : Errors[0];
		return false;
	}
	return true;
}

bool ExpandSelectedPathsWithRenamePairs(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InSelectedFiles,
	TArray<FString>& OutExpandedFiles, TArray<FGitRenamePair>& OutRenamePairs, TArray<FString>& OutErrorMessages)
{
	OutExpandedFiles.Reset();
	OutRenamePairs.Reset();
	if (InRepositoryRoot.IsEmpty() || InSelectedFiles.IsEmpty())
	{
		OutErrorMessages.Add(TEXT("Rename expansion requires explicit selected files."));
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TArray<FString> SelectedFiles;
	TSet<FString> SelectedKeys;
	for (const FString& InFile : InSelectedFiles)
	{
		if (FPaths::DirectoryExists(InFile))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Rename expansion does not accept directories: %s"), *InFile));
			return false;
		}
		FString File = FPaths::ConvertRelativePathToFull(InFile);
		FPaths::NormalizeFilename(File);
		if (!FPaths::IsUnderDirectory(File, RepositoryRoot))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Path is outside the Git repository: %s"), *File));
			return false;
		}
		const FString Key = NormalizeFileKey(File);
		if (!SelectedKeys.Contains(Key))
		{
			SelectedKeys.Add(Key);
			SelectedFiles.Add(File);
			OutExpandedFiles.Add(File);
		}
	}

	// A path-scoped status query cannot discover the counterpart of a rename when
	// only one side is selected. Use one local, repository-wide diff against HEAD
	// as the explicit mutation preflight. This reads no remote state and does not
	// add a second status or ls-files query.
	TArray<uint8> RenameOutput;
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot,
		// Manual port of upstream 9d5f309 intent: keep machine-parsed rename records free of ANSI sequences.
		TEXT("--no-optional-locks --literal-pathspecs diff --no-color --name-status -z --find-renames --diff-filter=R HEAD --"), RenameOutput, OutErrorMessages))
	{
		return false;
	}

	TSet<FString> ExpandedKeys = SelectedKeys;
	int32 Offset = 0;
	FString StatusToken;
	while (ReadNulToken(RenameOutput, Offset, StatusToken))
	{
		if (StatusToken.Len() < 2 || StatusToken[0] != TEXT('R'))
		{
			continue;
		}

		FString OldRelativePath;
		FString NewRelativePath;
		if (!ReadNulToken(RenameOutput, Offset, OldRelativePath) || !ReadNulToken(RenameOutput, Offset, NewRelativePath) ||
			OldRelativePath.IsEmpty() || NewRelativePath.IsEmpty())
		{
			OutErrorMessages.Add(TEXT("Malformed Git rename record."));
			return false;
		}

		FGitRenamePair Pair;
		Pair.OldPath = MakeAbsoluteStatusPath(RepositoryRoot, OldRelativePath);
		Pair.NewPath = MakeAbsoluteStatusPath(RepositoryRoot, NewRelativePath);
		const FString OldKey = NormalizeFileKey(Pair.OldPath);
		const FString NewKey = NormalizeFileKey(Pair.NewPath);
		if (!SelectedKeys.Contains(OldKey) && !SelectedKeys.Contains(NewKey))
		{
			continue;
		}
		OutRenamePairs.Add(Pair);
		if (!ExpandedKeys.Contains(OldKey))
		{
			ExpandedKeys.Add(OldKey);
			OutExpandedFiles.Add(Pair.OldPath);
		}
		if (!ExpandedKeys.Contains(NewKey))
		{
			ExpandedKeys.Add(NewKey);
			OutExpandedFiles.Add(Pair.NewPath);
		}
	}
	if (!AppendUnstagedRenameFallbackPairs(InPathToGitBinary, RepositoryRoot, SelectedKeys, ExpandedKeys, OutExpandedFiles, OutRenamePairs, OutErrorMessages))
	{
		return false;
	}
	return true;
}

bool CaptureIndexEntriesForPaths(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles,
	FGitIndexSnapshot& OutSnapshot, FString& OutError)
{
	OutSnapshot = FGitIndexSnapshot();
	OutError.Reset();
	if (InRepositoryRoot.IsEmpty() || InFiles.IsEmpty())
	{
		OutError = TEXT("Index snapshots require explicit file paths.");
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TSet<FString> SeenPaths;
	for (const FString& InFile : InFiles)
	{
		if (FPaths::DirectoryExists(InFile))
		{
			OutError = FString::Printf(TEXT("Index snapshots do not accept directories: %s"), *InFile);
			return false;
		}
		FString File = FPaths::ConvertRelativePathToFull(InFile);
		FPaths::NormalizeFilename(File);
		if (!FPaths::IsUnderDirectory(File, RepositoryRoot))
		{
			OutError = FString::Printf(TEXT("Path is outside the Git repository: %s"), *File);
			return false;
		}
		const FString FileKey = NormalizeFileKey(File);
		if (!SeenPaths.Contains(FileKey))
		{
			SeenPaths.Add(FileKey);
			OutSnapshot.Paths.Add(File);
		}
	}

	TArray<FString> Errors;
	TArray<uint8> StageOutput;
	if (!RunLsFilesCommands(InPathToGitBinary, RepositoryRoot, OutSnapshot.Paths, TEXT("--stage"), StageOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Failed to read Git index entries.") : Errors[0];
		return false;
	}

	TArray<uint8> ObjectFormatOutput;
	Errors.Reset();
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot, TEXT("rev-parse --show-object-format"), ObjectFormatOutput, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Failed to determine Git object format.") : Errors[0];
		return false;
	}
	FString ObjectFormat;
	FFileHelper::BufferToString(ObjectFormat, ObjectFormatOutput.GetData(), ObjectFormatOutput.Num());
	ObjectFormat.TrimStartAndEndInline();
	const int32 ObjectIdLength = ObjectFormat == TEXT("sha256") ? 64 : 40;

	TSet<FString> IndexedPaths;
	int32 Offset = 0;
	FString Record;
	while (ReadNulToken(StageOutput, Offset, Record))
	{
		int32 TabIndex = INDEX_NONE;
		if (Record.FindChar(TEXT('\t'), TabIndex))
		{
			IndexedPaths.Add(NormalizeFileKey(MakeAbsoluteStatusPath(RepositoryRoot, Record.Mid(TabIndex + 1))));
		}
	}

	OutSnapshot.RepositoryRoot = RepositoryRoot;
	OutSnapshot.IndexInfo = MoveTemp(StageOutput);
	const FString ZeroObjectId = FString::ChrN(ObjectIdLength, TEXT('0'));
	for (const FString& File : OutSnapshot.Paths)
	{
		if (IndexedPaths.Contains(NormalizeFileKey(File)))
		{
			continue;
		}
		FString RelativeFile = File;
		if (!MakeRepositoryRelativePath(RepositoryRoot, RelativeFile))
		{
			OutError = FString::Printf(TEXT("Path is outside the Git repository: %s"), *File);
			return false;
		}
		FPaths::NormalizeFilename(RelativeFile);
		const FString RemovalRecord = FString::Printf(TEXT("0 %s 0\t%s"), *ZeroObjectId, *RelativeFile);
		FTCHARToUTF8 Utf8(*RemovalRecord);
		OutSnapshot.IndexInfo.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		OutSnapshot.IndexInfo.Add(0);
	}
	return true;
}

bool RestoreIndexEntries(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FGitIndexSnapshot& InSnapshot, FString& OutError)
{
	OutError.Reset();
	if (InSnapshot.Paths.IsEmpty() || InSnapshot.IndexInfo.IsEmpty())
	{
		OutError = TEXT("Index snapshot is empty.");
		return false;
	}
	const FString RepositoryRoot = NormalizeRepositoryKey(InRepositoryRoot);
	if (RepositoryRoot != NormalizeRepositoryKey(InSnapshot.RepositoryRoot))
	{
		OutError = TEXT("Index snapshot belongs to a different Git repository.");
		return false;
	}
	FString ObjectFormatOutput;
	FString ObjectFormatErrors;
	if (!RunCommandInternalRaw(TEXT("rev-parse"), InPathToGitBinary, InSnapshot.RepositoryRoot,
		{ TEXT("--show-object-format") }, GetEmptyStringArray(), ObjectFormatOutput, ObjectFormatErrors, 0, false))
	{
		OutError = ObjectFormatErrors.IsEmpty() ? TEXT("Could not determine the Git object format for index rollback.") : ObjectFormatErrors;
		return false;
	}
	ObjectFormatOutput.TrimStartAndEndInline();
	const int32 ObjectIdLength = ObjectFormatOutput == TEXT("sha256") ? 64 : 40;
	const FString ZeroObjectId = FString::ChrN(ObjectIdLength, TEXT('0'));
	TArray<uint8> RestoreInput;
	for (const FString& File : InSnapshot.Paths)
	{
		FString RelativeFile = File;
		if (!MakeRepositoryRelativePath(InSnapshot.RepositoryRoot, RelativeFile))
		{
			OutError = FString::Printf(TEXT("Index snapshot path is outside the repository: %s"), *File);
			return false;
		}
		FPaths::NormalizeFilename(RelativeFile);
		const FString RemovalRecord = FString::Printf(TEXT("0 %s 0\t%s"), *ZeroObjectId, *RelativeFile);
		FTCHARToUTF8 Utf8(*RemovalRecord);
		RestoreInput.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		RestoreInput.Add(0);
	}
	RestoreInput.Append(InSnapshot.IndexInfo);

	const FGitProcessResult Result = ExecuteGitProcessOffGameThreadWithInput(InPathToGitBinary, InSnapshot.RepositoryRoot,
		TEXT("update-index -z --index-info"), RestoreInput);
	if (Result.bCancelled)
	{
		OutError = TEXT("Git index restore was cancelled.");
		return false;
	}
	if (Result.bTimedOut)
	{
		OutError = FString::Printf(TEXT("Git index restore timed out after %.0f seconds."), GitCommandTimeoutSeconds);
		return false;
	}
	if (Result.bInputWriteFailed || Result.ReturnCode != 0)
	{
		OutError = BytesToString(Result.StandardError);
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Git index restore failed.");
		}
		return false;
	}
	return true;
}

bool RunExactPathspecMutation(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InVerb,
	const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutError)
{
	OutError.Reset();
	const bool bRestore = InVerb == TEXT("restore");
	const bool bRemoveCached = InVerb == TEXT("rm");
	const bool bReset = InVerb == TEXT("reset");
	if ((!bRestore && !bRemoveCached && !bReset) || InFiles.IsEmpty())
	{
		OutError = TEXT("Only explicit local Git restore, index reset, or cached-index removal mutations are supported.");
		return false;
	}
	if (InParameters.Num() != (bRestore ? 3 : 2))
	{
		OutError = TEXT("The requested Git mutation parameters are not permitted.");
		return false;
	}
	TSet<FString> RequestedParameters;
	for (const FString& Parameter : InParameters)
	{
		const bool bAllowed = bRestore
			? Parameter == TEXT("--source=HEAD") || Parameter == TEXT("--staged") || Parameter == TEXT("--worktree")
			: bReset
				? Parameter == TEXT("-q") || Parameter == TEXT("HEAD")
				: Parameter == TEXT("--cached") || Parameter == TEXT("--ignore-unmatch");
		if (!bAllowed || RequestedParameters.Contains(Parameter))
		{
			OutError = TEXT("The requested Git mutation parameters are not permitted.");
			return false;
		}
		RequestedParameters.Add(Parameter);
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TArray<FString> Files;
	TSet<FString> SeenPaths;
	for (const FString& InFile : InFiles)
	{
		if (FPaths::DirectoryExists(InFile))
		{
			OutError = FString::Printf(TEXT("Git restore does not accept directories: %s"), *InFile);
			return false;
		}
		FString File = FPaths::ConvertRelativePathToFull(InFile);
		FPaths::NormalizeFilename(File);
		if (!FPaths::IsUnderDirectory(File, RepositoryRoot))
		{
			OutError = FString::Printf(TEXT("Path is outside the Git repository: %s"), *File);
			return false;
		}
		const FString FileKey = NormalizeFileKey(File);
		if (!SeenPaths.Contains(FileKey))
		{
			SeenPaths.Add(FileKey);
			Files.Add(File);
		}
	}

	FString Arguments;
	if (!BuildLiteralPathArguments(RepositoryRoot, InVerb, InParameters, Files, Arguments, OutError))
	{
		return false;
	}
	TArray<uint8> Output;
	TArray<FString> Errors;
	if (!RunLocalCommand(InPathToGitBinary, RepositoryRoot, Arguments, Output, Errors))
	{
		OutError = Errors.IsEmpty() ? TEXT("Git mutation failed.") : Errors[0];
		return false;
	}
	return true;
}

// Run a local, path-scoped Git status query. Directories and empty scopes are deliberately rejected.
bool RunUpdateStatus(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
					 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlFileStatus>& OutStates)
{
	static_cast<void>(InUsingLfsLocking);
	OutStates.Reset();
	if (InRepositoryRoot.IsEmpty() || InFiles.IsEmpty())
	{
		OutErrorMessages.Add(TEXT("Local Git status requires one or more explicit file paths."));
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	TArray<FString> Files;
	TSet<FString> SeenFiles;
	for (const FString& InFile : InFiles)
	{
		if (FPaths::DirectoryExists(InFile))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Directory status is not supported; refresh explicit assets instead: %s"), *InFile));
			return false;
		}
		FString File = FPaths::ConvertRelativePathToFull(InFile);
		FPaths::NormalizeFilename(File);
		if (!FPaths::IsUnderDirectory(File, RepositoryRoot))
		{
			OutErrorMessages.Add(FString::Printf(TEXT("Path is outside the Git repository: %s"), *File));
			return false;
		}
		const FString Key = NormalizeFileKey(File);
		if (!SeenFiles.Contains(Key))
		{
			SeenFiles.Add(Key);
			Files.Add(File);
		}
	}

	TArray<uint8> StatusOutput;
	if (!RunStatusCommands(InPathToGitBinary, RepositoryRoot, Files, StatusOutput, OutErrorMessages))
	{
		return false;
	}

	TArray<uint8> TrackedOutput;
	if (!RunLsFilesCommands(InPathToGitBinary, RepositoryRoot, Files, FString(), TrackedOutput, OutErrorMessages))
	{
		return false;
	}

	TMap<FString, FGitSourceControlFileStatus> ParsedStates;
	ParsePorcelainV2Status(StatusOutput, RepositoryRoot, ParsedStates);
	TSet<FString> TrackedPaths;
	ParseTrackedPaths(TrackedOutput, RepositoryRoot, TrackedPaths);
	for (const FString& File : Files)
	{
		const FString Key = NormalizeFileKey(File);
		if (FGitSourceControlFileStatus* ParsedState = ParsedStates.Find(Key))
		{
			OutStates.Add(File, MoveTemp(*ParsedState));
			continue;
		}

		FGitSourceControlFileStatus State;
		State.FileState = EGitFileState::Unknown;
		State.TreeState = TrackedPaths.Contains(Key) ? EGitTreeState::Unmodified : EGitTreeState::NotInRepo;
		OutStates.Add(File, MoveTemp(State));
	}

	return true;
}


/** Translate file actions from the given Git log --name-status command to keywords used by the Editor UI.
 *
 * @see https://www.kernel.org/pub/software/scm/git/docs/git-log.html
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'T' = type changed
 * 'U' = updated but unmerged
 * 'X' = unknown
 * 'B' = broken pairing
 *
 * @see SHistoryRevisionListRowContent::GenerateWidgetForColumn(): "add", "edit", "delete", "branch" and "integrate" (everything else is taken like "edit")
 */
static FString LogStatusToString(TCHAR InStatus)
{
	switch (InStatus)
	{
		case TEXT(' '):
			return FString("unmodified");
		case TEXT('M'):
			return FString("modified");
		case TEXT('A'): // added: keyword "add" to display a specific icon instead of the default "edit" action one
			return FString("add");
		case TEXT('D'): // deleted: keyword "delete" to display a specific icon instead of the default "edit" action one
			return FString("delete");
		case TEXT('R'): // renamed keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('C'): // copied keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('T'):
			return FString("type changed");
		case TEXT('U'):
			return FString("unmerged");
		case TEXT('X'):
			return FString("unknown");
		case TEXT('B'):
			return FString("broked pairing");
	}

	return FString();
}

static bool ParseMachineHistory(const TArray<uint8>& InOutput, const FString& InGitBinary, const FString& InRepositoryRoot, TGitSourceControlHistory& OutHistory)
{
	TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> CurrentRevision;
	int32 Offset = 0;
	FString Token;
	while (ReadNulToken(InOutput, Offset, Token))
	{
		// BufferToString can preserve an UTF-8 BOM as U+FEFF. It is the only
		// prefix accepted here; 0x1e is Git's record separator and must remain
		// the first character of a commit header.
		if (!Token.IsEmpty() && Token[0] == static_cast<TCHAR>(0xFEFF))
		{
			Token.RightChopInline(1);
		}
		if (Token.IsEmpty())
		{
			continue;
		}

		if (Token[0] == static_cast<TCHAR>(0x1e))
		{
			if (CurrentRevision.IsValid())
			{
				OutHistory.Add(CurrentRevision.ToSharedRef());
			}

			TArray<FString> Fields;
			Token.Mid(1).ParseIntoArray(Fields, TEXT("\x1f"), false);
			if (Fields.Num() != 4 || Fields[0].IsEmpty())
			{
				CurrentRevision.Reset();
				continue;
			}

			CurrentRevision = MakeShared<FGitSourceControlRevision, ESPMode::ThreadSafe>();
			CurrentRevision->CommitId = Fields[0];
			CurrentRevision->ShortCommitId = CurrentRevision->CommitId.Left(8);
			CurrentRevision->CommitIdNumber = FParse::HexNumber(*CurrentRevision->ShortCommitId);
			CurrentRevision->UserName = Fields[1];
			CurrentRevision->Date = FDateTime::FromUnixTimestamp(FCString::Atoi64(*Fields[2]));
			CurrentRevision->Description = Fields[3];
			CurrentRevision->FileSize = 0;
			CurrentRevision->GitBinary = InGitBinary;
			CurrentRevision->RepositoryRoot = InRepositoryRoot;
			continue;
		}

		if (!CurrentRevision.IsValid())
		{
			continue;
		}

		// `git log --format=...%x00 --name-status -z` emits an extra NUL
		// followed by a line break before the first name-status token (for
		// example: 00 00 0a 4d 00). Remove only that protocol separator;
		// path names and commit headers must remain byte-for-byte intact.
		if (Token.StartsWith(TEXT("\r\n")))
		{
			Token.RightChopInline(2);
		}
		else if (Token.StartsWith(TEXT("\n")) || Token.StartsWith(TEXT("\r")))
		{
			Token.RightChopInline(1);
		}

		const TCHAR Status = Token[0];
		if (Status != TEXT('A') && Status != TEXT('M') && Status != TEXT('D') && Status != TEXT('R') && Status != TEXT('C') && Status != TEXT('T') && Status != TEXT('U'))
		{
			continue;
		}

		FString FirstPath;
		if (!ReadNulToken(InOutput, Offset, FirstPath))
		{
			return false;
		}
		CurrentRevision->Action = LogStatusToString(Status);
		CurrentRevision->Filename = FirstPath;
		CurrentRevision->LocalFilename = MakeAbsoluteStatusPath(InRepositoryRoot, FirstPath);
		if (Status == TEXT('R') || Status == TEXT('C'))
		{
			FString NewPath;
			if (!ReadNulToken(InOutput, Offset, NewPath))
			{
				return false;
			}
			CurrentRevision->Filename = NewPath;
			CurrentRevision->LocalFilename = MakeAbsoluteStatusPath(InRepositoryRoot, NewPath);
		}
	}

	if (CurrentRevision.IsValid())
	{
		OutHistory.Add(CurrentRevision.ToSharedRef());
	}
	for (int32 Index = 0; Index < OutHistory.Num(); ++Index)
	{
		OutHistory[Index]->RevisionNumber = OutHistory.Num() - Index;
		if (OutHistory[Index]->Action == TEXT("branch") && OutHistory.IsValidIndex(Index + 1))
		{
			OutHistory[Index]->BranchSource = OutHistory[Index + 1];
		}
	}
	return OutHistory.Num() > 0;
}

namespace GitSourceControlHistoryPrivate
{
constexpr int32 MaxHistoryEntries = 250;

bool ResolveHead(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutHead, TArray<FString>& OutErrorMessages)
{
	OutHead.Reset();
	TArray<uint8> Output;
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, TEXT("--no-optional-locks rev-parse --verify HEAD"), Output, OutErrorMessages))
	{
		return false;
	}
	OutHead = BytesToString(Output);
	OutHead.TrimStartAndEndInline();
	if (OutHead.IsEmpty())
	{
		OutErrorMessages.Add(TEXT("Git did not return HEAD for the history snapshot."));
		return false;
	}
	return true;
}

bool LoadSegment(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InStartCommit, const FString& InRelativePath,
	const FString& InLocalFilename, const int32 InMaxCount, TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory)
{
	TArray<uint8> Output;
	// Manual port of upstream 9d5f309 intent: keep machine-parsed history records free of ANSI sequences.
	FString Arguments = FString::Printf(TEXT("--no-optional-locks --literal-pathspecs log --no-color --date=raw --name-status -z --max-count=%d --format=%%x1e%%H%%x1f%%an%%x1f%%at%%x1f%%s%%x00"), InMaxCount);
	AppendGitArgument(Arguments, InStartCommit);
	Arguments += TEXT(" --");
	AppendGitArgument(Arguments, InRelativePath);
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, Arguments, Output, OutErrorMessages))
	{
		return false;
	}

	TGitSourceControlHistory Segment;
	if (Output.IsEmpty())
	{
		return true;
	}
	if (!ParseMachineHistory(Output, InPathToGitBinary, InRepositoryRoot, Segment))
	{
		OutErrorMessages.Add(TEXT("Git returned malformed history for the selected file."));
		return false;
	}
	for (int32 Index = 0; Index < Segment.Num(); ++Index)
	{
		if (Segment[Index]->Action == TEXT("add"))
		{
			// Git path history 否则会跨过 delete/re-add, 把同路径的旧无关文件附加进来.
			// 从新到旧遇到的第一个 add 是当前 path segment 的 birth boundary.
			Segment.SetNum(Index + 1);
			break;
		}
	}
	for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : Segment)
	{
		// UE History widget 使用稳定的当前 workspace path; Filename 保留该 commit 的真实 blob path.
		Revision->Filename = InRelativePath;
		Revision->LocalFilename = InLocalFilename;
		Revision->BranchSource.Reset();
	}
	OutHistory.Append(MoveTemp(Segment));
	return true;
}

bool FindExactRenamePredecessor(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InCommit,
	const FString& InExpectedNewPath, FString& OutParentCommit, FString& OutOldPath, TArray<FString>& OutErrorMessages)
{
	OutParentCommit.Reset();
	OutOldPath.Reset();

	TArray<uint8> ParentOutput;
	FString ParentArguments = TEXT("--no-optional-locks rev-list --parents -n 1");
	AppendGitArgument(ParentArguments, InCommit);
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, ParentArguments, ParentOutput, OutErrorMessages))
	{
		return false;
	}
	TArray<FString> ParentFields;
	BytesToString(ParentOutput).ParseIntoArrayWS(ParentFields);
	if (ParentFields.Num() != 2 || !ParentFields[0].Equals(InCommit, ESearchCase::IgnoreCase))
	{
		// root 和 merge 是有意的边界, 两者都无法证明线性的 asset path.
		return true;
	}

	TArray<uint8> DiffOutput;
	// Manual port of upstream 9d5f309 intent: keep machine-parsed rename records free of ANSI sequences.
	FString DiffArguments = TEXT("--no-optional-locks diff-tree --no-color --no-commit-id -r --name-status -z --find-renames=100%");
	AppendGitArgument(DiffArguments, ParentFields[1]);
	AppendGitArgument(DiffArguments, InCommit);
	if (!RunLocalCommand(InPathToGitBinary, InRepositoryRoot, DiffArguments, DiffOutput, OutErrorMessages))
	{
		return false;
	}

	int32 Offset = 0;
	FString Status;
	while (ReadNulToken(DiffOutput, Offset, Status))
	{
		const bool bRenameOrCopy = Status.StartsWith(TEXT("R"), ESearchCase::CaseSensitive) || Status.StartsWith(TEXT("C"), ESearchCase::CaseSensitive);
		if (!Status.Equals(TEXT("R100"), ESearchCase::CaseSensitive))
		{
			FString IgnoredPath;
			if (!ReadNulToken(DiffOutput, Offset, IgnoredPath))
			{
				OutErrorMessages.Add(TEXT("Git returned an incomplete commit diff while checking an exact rename."));
				return false;
			}
			if (bRenameOrCopy && !ReadNulToken(DiffOutput, Offset, IgnoredPath))
			{
				OutErrorMessages.Add(TEXT("Git returned an incomplete rename or copy record while checking an exact rename."));
				return false;
			}
			continue;
		}

		FString OldPath;
		FString NewPath;
		if (!ReadNulToken(DiffOutput, Offset, OldPath) || !ReadNulToken(DiffOutput, Offset, NewPath))
		{
			OutErrorMessages.Add(TEXT("Git returned an incomplete exact rename record."));
			return false;
		}
		if (NewPath.Equals(InExpectedNewPath, ESearchCase::CaseSensitive))
		{
			OutParentCommit = ParentFields[1];
			OutOldPath = MoveTemp(OldPath);
			return true;
		}
	}
	return true;
}
}

// 从固定 HEAD snapshot 查询单文件. ExactRenames 逐 commit 只追踪已提交 R100 move.
bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, const bool bMergeConflict,
	const EGitLocalSourceControlHistoryMode InMode, FString& OutCapturedHead, bool& bOutHeadChanged, TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory)
{
	OutHistory.Reset();
	OutCapturedHead.Reset();
	bOutHeadChanged = false;
	if (bMergeConflict)
	{
		OutErrorMessages.Add(TEXT("History for conflicted files is unavailable. Resolve the conflict in an external Git GUI first."));
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	FString LocalFilename = FPaths::ConvertRelativePathToFull(InFile);
	FPaths::NormalizeFilename(LocalFilename);
	FString RelativeFile = LocalFilename;
	if (!MakeRepositoryRelativePath(RepositoryRoot, RelativeFile))
	{
		OutErrorMessages.Add(FString::Printf(TEXT("History path is outside the Git repository: %s"), *InFile));
		return false;
	}
	FPaths::NormalizeFilename(RelativeFile);

	if (!GitSourceControlHistoryPrivate::ResolveHead(InPathToGitBinary, RepositoryRoot, OutCapturedHead, OutErrorMessages))
	{
		return false;
	}

	FString SegmentStart = OutCapturedHead;
	FString SegmentPath = RelativeFile;
	TSet<FString> VisitedSegments;
	while (OutHistory.Num() < GitSourceControlHistoryPrivate::MaxHistoryEntries)
	{
		const FString SegmentKey = SegmentStart + TEXT("\n") + SegmentPath;
		if (VisitedSegments.Contains(SegmentKey))
		{
			break;
		}
		VisitedSegments.Add(SegmentKey);

		const int32 SegmentFirstIndex = OutHistory.Num();
		if (!GitSourceControlHistoryPrivate::LoadSegment(InPathToGitBinary, RepositoryRoot, SegmentStart, SegmentPath, LocalFilename,
			GitSourceControlHistoryPrivate::MaxHistoryEntries - SegmentFirstIndex, OutErrorMessages, OutHistory))
		{
			return false;
		}
		if (SegmentFirstIndex == OutHistory.Num())
		{
			break;
		}
		if (InMode == EGitLocalSourceControlHistoryMode::CurrentPath || OutHistory.Num() >= GitSourceControlHistoryPrivate::MaxHistoryEntries)
		{
			break;
		}

		const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& OldestRevision = OutHistory.Last();
		FString ParentCommit;
		FString OldPath;
		if (!GitSourceControlHistoryPrivate::FindExactRenamePredecessor(InPathToGitBinary, RepositoryRoot, OldestRevision->CommitId, SegmentPath, ParentCommit, OldPath, OutErrorMessages))
		{
			return false;
		}
		if (ParentCommit.IsEmpty() || OldPath.IsEmpty())
		{
			break;
		}
		SegmentStart = MoveTemp(ParentCommit);
		SegmentPath = MoveTemp(OldPath);
	}

	if (OutHistory.IsEmpty())
	{
		OutErrorMessages.Add(TEXT("Git returned no history for the selected file."));
		return false;
	}
	for (int32 Index = 0; Index < OutHistory.Num(); ++Index)
	{
		OutHistory[Index]->RevisionNumber = OutHistory.Num() - Index;
		OutHistory[Index]->BranchSource.Reset();
	}

	TArray<FString> HeadCheckErrors;
	FString HeadAtCompletion;
	if (GitSourceControlHistoryPrivate::ResolveHead(InPathToGitBinary, RepositoryRoot, HeadAtCompletion, HeadCheckErrors))
	{
		bOutHeadChanged = !HeadAtCompletion.Equals(OutCapturedHead, ESearchCase::IgnoreCase);
	}
	return true;
}

TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> RelativeFiles;
	for (FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		if (MakeRepositoryRelativePath(InRelativeTo, FileName))
		{
			RelativeFiles.Add(FileName);
		}
	}

	return RelativeFiles;
}

TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> AbsFiles;

	for(FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		AbsFiles.Add(FPaths::Combine(InRelativeTo, FileName));
	}

	return AbsFiles;
}

bool DumpRevisionBlobToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InRevisionSpec, const FString& InOutputFile, FString& OutError)
{
	OutError.Reset();
	if (InPathToGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || InRevisionSpec.IsEmpty() || InOutputFile.IsEmpty())
	{
		OutError = TEXT("Git binary, repository root, revision and output file are required.");
		return false;
	}

	const FString OutputDirectory = FPaths::GetPath(InOutputFile);
	if (OutputDirectory.IsEmpty())
	{
		OutError = TEXT("The revision output file must include a directory.");
		return false;
	}
	const FString TemporaryFile = FPaths::CreateTempFilename(*OutputDirectory, TEXT(".git-source-control-blob-"), TEXT(".tmp"));
	const FString Arguments = FString::Printf(TEXT("--no-optional-locks cat-file blob %s"), *QuoteGitArgument(InRevisionSpec));
	const FGitProcessResult ProcessResult = ExecuteGitProcessOffGameThread(InPathToGitBinary, InRepositoryRoot, Arguments);
	if (ProcessResult.bCancelled || ProcessResult.bTimedOut || ProcessResult.ReturnCode != 0)
	{
		IFileManager::Get().Delete(*TemporaryFile, false, true, true);
		OutError = ProcessResult.bCancelled
			? TEXT("Revision blob export was cancelled.")
			: ProcessResult.bTimedOut
				? TEXT("Revision blob export timed out.")
				: BytesToString(ProcessResult.StandardError);
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Git could not export the requested revision blob.");
		}
		return false;
	}
	if (!FFileHelper::SaveArrayToFile(ProcessResult.StandardOutput, *TemporaryFile) || !IFileManager::Get().Move(*InOutputFile, *TemporaryFile, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryFile, false, true, true);
		OutError = FString::Printf(TEXT("Could not atomically write revision blob '%s'."), *InOutputFile);
		return false;
	}
	return true;
}

} // namespace GitSourceControlUtils

#undef LOCTEXT_NAMESPACE
