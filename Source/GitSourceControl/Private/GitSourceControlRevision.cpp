// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlRevision.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

static bool IsLfsPointerFile(const FString& InFilename)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *InFilename))
	{
		return false;
	}

	const ANSICHAR* Signature = "version https://git-lfs.github.com/spec/v1";
	const int32 SignatureLen = FCStringAnsi::Strlen(Signature);
	if (Data.Num() < SignatureLen)
	{
		return false;
	}

	return FMemory::Memcmp(Data.GetData(), Signature, SignatureLen) == 0;
}

static bool IsValidUassetFile(const FString& InFilename)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *InFilename))
	{
		return false;
	}

	if (Data.Num() < sizeof(uint32))
	{
		return false;
	}

	const uint32 Tag = *reinterpret_cast<const uint32*>(Data.GetData());
	return Tag == PACKAGE_FILE_TAG || Tag == PACKAGE_FILE_TAG_SWAPPED;
}

static FString ReadFileHeaderHex(const FString& InFilename, int32 NumBytes = 16)
{
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *InFilename))
	{
		return TEXT("");
	}

	const int32 Count = FMath::Min(NumBytes, Data.Num());
	FString Hex;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Hex += FString::Printf(TEXT("%02X"), Data[Index]);
		if (Index + 1 < Count)
		{
			Hex += TEXT(" ");
		}
	}
	return Hex;
}

#if ENGINE_MAJOR_VERSION >= 5
bool FGitSourceControlRevision::Get( FString& InOutFilename, EConcurrency::Type InConcurrency ) const
{
	if (InConcurrency != EConcurrency::Synchronous)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Only EConcurrency::Synchronous is tested/supported for this operation."));
	}
#else
bool FGitSourceControlRevision::Get( FString& InOutFilename ) const
{
#endif
	const FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
	if (!GitSourceControl)
	{
		return false;
	}
	const FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
	const FString PathToGitBinary = Provider.GetGitBinaryPath();
	FString PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	// the repo root can be customised if in a plugin that has it's own repo
	if (PathToRepoRoot.Len())
	{
		PathToRepositoryRoot = PathToRepoRoot;
	}

	// if a filename for the temp file wasn't supplied generate a unique-ish one
	if(InOutFilename.Len() == 0)
	{
		// create the diff dir if we don't already have it (Git wont)
		IFileManager::Get().MakeDirectory(*FPaths::DiffDir(), true);
		// create a unique temp file name based on the unique commit Id
		const FString TempFileName = FString::Printf(TEXT("%stemp-%s-%s"), *FPaths::DiffDir(), *CommitId, *FPaths::GetCleanFilename(Filename));
		InOutFilename = FPaths::ConvertRelativePathToFull(TempFileName);
	}

	// Diff against the revision
	const FString Parameter = FString::Printf(TEXT("%s:%s"), *CommitId, *Filename);

	bool bCommandSuccessful;
	if(FPaths::FileExists(InOutFilename))
	{
		const bool bPointer = IsLfsPointerFile(InOutFilename);
		const bool bValidUasset = IsValidUassetFile(InOutFilename);
		if (!bPointer && bValidUasset)
		{
			bCommandSuccessful = true; // reuse only if the file looks valid
		}
		else
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Diff temp file invalid, deleting. File='%s' Pointer=%d ValidUasset=%d Header='%s'"),
				*InOutFilename, bPointer ? 1 : 0, bValidUasset ? 1 : 0, *ReadFileHeaderHex(InOutFilename));
			IFileManager::Get().Delete(*InOutFilename);
			bCommandSuccessful = false;
		}
	}
	else
	{
		bCommandSuccessful = false;
	}

	if (!bCommandSuccessful)
	{
		UE_LOG(LogSourceControl, Log, TEXT("Diff export start. File='%s' Commit='%s' Path='%s'"),
			*InOutFilename, *CommitId, *Filename);
		bCommandSuccessful = GitSourceControlUtils::RunDumpToFile(PathToGitBinary, PathToRepositoryRoot, Parameter, InOutFilename);
		if (bCommandSuccessful && IsLfsPointerFile(InOutFilename))
		{
			bCommandSuccessful = false;
		}

		if (!bCommandSuccessful)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Diff export failed or LFS pointer. File='%s' Header='%s'"),
				*InOutFilename, *ReadFileHeaderHex(InOutFilename));
			TArray<FString> LfsErrors;
			const bool bLfsFetchOk = GitSourceControlUtils::FetchLfsContentForFile(PathToGitBinary, PathToRepositoryRoot, Filename, LfsErrors);
			if (!bLfsFetchOk && !FApp::IsUnattended() && !IsRunningCommandlet())
			{
				const FText Title = LOCTEXT("GitLfsFetchFailedTitle", "Git LFS Fetch Failed");
				const FText Message = FText::Format(
					LOCTEXT("GitLfsFetchFailedMessage", "Failed to fetch Git LFS content for file:\n{0}\n\nPlease run 'git lfs fetch --all' or check your network."),
					FText::FromString(Filename));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
				FMessageDialog::Open(EAppMsgType::Ok, Message, Title);
#else
				FMessageDialog::Open(EAppMsgType::Ok, Message, &Title);
#endif
			}
			if (bLfsFetchOk)
			{
				UE_LOG(LogSourceControl, Log, TEXT("Diff export retry after LFS fetch. File='%s'"), *InOutFilename);
				bCommandSuccessful = GitSourceControlUtils::RunDumpToFile(PathToGitBinary, PathToRepositoryRoot, Parameter, InOutFilename);
				if (bCommandSuccessful && IsLfsPointerFile(InOutFilename))
				{
					bCommandSuccessful = false;
				}
			}
		}

		if (bCommandSuccessful)
		{
			const bool bValidUasset = IsValidUassetFile(InOutFilename);
			if (!bValidUasset)
			{
				UE_LOG(LogSourceControl, Error, TEXT("Diff export invalid uasset, keep for debug. File='%s' Header='%s'"),
					*InOutFilename, *ReadFileHeaderHex(InOutFilename));
				bCommandSuccessful = false;
			}
		}

		if (!bCommandSuccessful && !FApp::IsUnattended() && !IsRunningCommandlet())
		{
			const FText Title = LOCTEXT("GitLfsDumpFailedTitle", "Git LFS Export Failed");
			const FText Message = FText::Format(
				LOCTEXT("GitLfsDumpFailedMessage", "Failed to export file revision for diff:\n{0}\n\nPlease run 'git lfs fetch --all' and try again. If the file is a Git LFS asset, ensure git-lfs is available in PATH."),
				FText::FromString(Filename));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
			FMessageDialog::Open(EAppMsgType::Ok, Message, Title);
#else
			FMessageDialog::Open(EAppMsgType::Ok, Message, &Title);
#endif
		}
	}
	return bCommandSuccessful;
}

bool FGitSourceControlRevision::GetAnnotated( TArray<FAnnotationLine>& OutLines ) const
{
	return false;
}

bool FGitSourceControlRevision::GetAnnotated( FString& InOutFilename ) const
{
	return false;
}

const FString& FGitSourceControlRevision::GetFilename() const
{
	return Filename;
}

int32 FGitSourceControlRevision::GetRevisionNumber() const
{
	return RevisionNumber;
}

const FString& FGitSourceControlRevision::GetRevision() const
{
	return ShortCommitId;
}

const FString& FGitSourceControlRevision::GetDescription() const
{
	return Description;
}

const FString& FGitSourceControlRevision::GetUserName() const
{
	return UserName;
}

const FString& FGitSourceControlRevision::GetClientSpec() const
{
	static FString EmptyString(TEXT(""));
	return EmptyString;
}

const FString& FGitSourceControlRevision::GetAction() const
{
	return Action;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlRevision::GetBranchSource() const
{
	// if this revision was copied/moved from some other revision
	return BranchSource;
}

const FDateTime& FGitSourceControlRevision::GetDate() const
{
	return Date;
}

int32 FGitSourceControlRevision::GetCheckInIdentifier() const
{
	return CommitIdNumber;
}

int32 FGitSourceControlRevision::GetFileSize() const
{
	return FileSize;
}

#undef LOCTEXT_NAMESPACE
