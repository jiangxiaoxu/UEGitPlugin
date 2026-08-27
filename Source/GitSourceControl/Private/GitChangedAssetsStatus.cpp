// Copyright (c) 2026

#include "GitChangedAssetsStatus.h"

#include "GitSourceControlUtils.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GitChangedAssetsStatusPrivate
{
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

	bool MakeAbsoluteAssetPath(const FString& InRepositoryRoot, const FString& InRepositoryRelativePath, FString& OutAbsolutePath)
	{
		OutAbsolutePath.Reset();
		if (InRepositoryRelativePath.IsEmpty() || InRepositoryRelativePath == TEXT(".") || InRepositoryRelativePath == TEXT("..") ||
			InRepositoryRelativePath.StartsWith(TEXT("../"), ESearchCase::CaseSensitive) || FPaths::IsRelative(InRepositoryRelativePath) == false)
		{
			return false;
		}

		FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		FPaths::NormalizeDirectoryName(RepositoryRoot);
		FString RepositoryRootWithSlash = RepositoryRoot;
		if (!RepositoryRootWithSlash.EndsWith(TEXT("/")))
		{
			RepositoryRootWithSlash += TEXT("/");
		}
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(RepositoryRoot, InRepositoryRelativePath);
		FPaths::NormalizeFilename(AbsolutePath);
		if (!FPaths::IsUnderDirectory(AbsolutePath, RepositoryRootWithSlash))
		{
			return false;
		}
		OutAbsolutePath = MoveTemp(AbsolutePath);
		return true;
	}

	bool ReadHeadCommitId(const FString& InGitBinary, const FString& InRepositoryRoot, FString& OutHead, FString& OutError)
	{
		OutHead.Reset();
		OutError.Reset();
		FString StandardOutput;
		if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), InGitBinary, InRepositoryRoot,
			{ TEXT("--verify"), TEXT("HEAD") }, {}, StandardOutput, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Could not resolve repository HEAD.");
			}
			return false;
		}
		StandardOutput.TrimStartAndEndInline();
		if (StandardOutput.IsEmpty())
		{
			OutError = TEXT("Git did not return a HEAD commit.");
			return false;
		}
		OutHead = MoveTemp(StandardOutput);
		return true;
	}

	EGitChangedAssetState ResolveState(const TCHAR InRecordType, const TCHAR InIndexStatus, const TCHAR InWorktreeStatus)
	{
		if (InRecordType == TEXT('u') || InIndexStatus == TEXT('U') || InWorktreeStatus == TEXT('U') ||
			(InIndexStatus == TEXT('A') && InWorktreeStatus == TEXT('A')) || (InIndexStatus == TEXT('D') && InWorktreeStatus == TEXT('D')))
		{
			return EGitChangedAssetState::Conflicted;
		}
		if (InIndexStatus == TEXT('C'))
		{
			// Copy 相对 HEAD 引入新路径, 原路径不属于该行 Revert-to-HEAD 闭包.
			return EGitChangedAssetState::Added;
		}
		if (InRecordType == TEXT('2') || InIndexStatus == TEXT('R'))
		{
			return EGitChangedAssetState::Renamed;
		}
		if (InRecordType == TEXT('?'))
		{
			return EGitChangedAssetState::Untracked;
		}
		if (InIndexStatus == TEXT('A'))
		{
			return EGitChangedAssetState::Added;
		}
		if (InIndexStatus == TEXT('D') || InWorktreeStatus == TEXT('D'))
		{
			return EGitChangedAssetState::Deleted;
		}
		return EGitChangedAssetState::Modified;
	}

	bool ParseXY(const FString& InRecord, const TCHAR InRecordType, TCHAR& OutIndexStatus, TCHAR& OutWorktreeStatus, FString& OutError)
	{
		if (InRecordType == TEXT('?'))
		{
			OutIndexStatus = TEXT('?');
			OutWorktreeStatus = TEXT('?');
			return true;
		}
		if (InRecord.Len() < 4 || InRecord[1] != TEXT(' '))
		{
			OutError = TEXT("Malformed porcelain-v2 status record.");
			return false;
		}
		OutIndexStatus = InRecord[2];
		OutWorktreeStatus = InRecord[3];
		return true;
	}

	FString NormalizeEntryPathKey(const FString& InAbsoluteFilename)
	{
		FString Result = InAbsoluteFilename;
		FPaths::NormalizeFilename(Result);
#if PLATFORM_WINDOWS
		Result.ToLowerInline();
#endif
		return Result;
	}

	bool IsStagedDeletionWithUntrackedReplacement(const FGitChangedAssetEntry& InEntry)
	{
		return InEntry.State == EGitChangedAssetState::Deleted
			&& InEntry.IndexStatus == TEXT('D')
			&& InEntry.WorktreeStatus == TEXT('.');
	}

	bool AddOrMergeEntry(FGitChangedAssetEntry&& InEntry, TMap<FString, int32>& InOutEntryIndicesByPath,
		TArray<FGitChangedAssetEntry>& OutEntries, FString& OutError)
	{
		const FString PathKey = NormalizeEntryPathKey(InEntry.AbsoluteFilename);
		if (const int32* ExistingIndex = InOutEntryIndicesByPath.Find(PathKey))
		{
			FGitChangedAssetEntry& Existing = OutEntries[*ExistingIndex];
			const bool bExistingIsUntracked = Existing.State == EGitChangedAssetState::Untracked;
			const bool bIncomingIsUntracked = InEntry.State == EGitChangedAssetState::Untracked;
			const FGitChangedAssetEntry& TrackedEntry = bExistingIsUntracked ? InEntry : Existing;
			if ((!bExistingIsUntracked && !bIncomingIsUntracked) || !IsStagedDeletionWithUntrackedReplacement(TrackedEntry))
			{
				OutError = FString::Printf(TEXT("Git returned an unsupported duplicate status topology for one Changed Asset path: %s"), *InEntry.RepositoryRelativePath);
				return false;
			}

			// staged deletion 可与同路径 untracked replacement 共存. 保留 tracked
			// record 及其准确 XY 作为 Revert-to-HEAD 基线, replacement 仅保留拓扑信息.
			if (bExistingIsUntracked)
			{
				InEntry.bHasUntrackedReplacement = true;
				Existing = MoveTemp(InEntry);
			}
			else
			{
				Existing.bHasUntrackedReplacement = true;
			}
			return true;
		}

		InOutEntryIndicesByPath.Add(PathKey, OutEntries.Num());
		OutEntries.Add(MoveTemp(InEntry));
		return true;
	}

	bool AddEntry(const FString& InRepositoryRoot, const TCHAR InRecordType, const FString& InRecord, const FString& InCurrentPath,
		const FString& InOriginalPath, TMap<FString, int32>& InOutEntryIndicesByPath, TArray<FGitChangedAssetEntry>& OutEntries, FString& OutError)
	{
		if (!IsGitChangedAssetUassetPath(InCurrentPath))
		{
			return true;
		}

		FGitChangedAssetEntry Entry;
		Entry.RepositoryRelativePath = InCurrentPath;
		FPaths::NormalizeFilename(Entry.RepositoryRelativePath);
		if (!MakeAbsoluteAssetPath(InRepositoryRoot, Entry.RepositoryRelativePath, Entry.AbsoluteFilename))
		{
			OutError = FString::Printf(TEXT("Git returned an invalid repository-relative asset path: %s"), *InCurrentPath);
			return false;
		}
		if (!ParseXY(InRecord, InRecordType, Entry.IndexStatus, Entry.WorktreeStatus, OutError))
		{
			return false;
		}
		Entry.State = ResolveState(InRecordType, Entry.IndexStatus, Entry.WorktreeStatus);

		if (InRecordType == TEXT('2') && Entry.IsRename())
		{
			Entry.RenameFromRepositoryRelativePath = InOriginalPath;
			FPaths::NormalizeFilename(Entry.RenameFromRepositoryRelativePath);
			if (!MakeAbsoluteAssetPath(InRepositoryRoot, Entry.RenameFromRepositoryRelativePath, Entry.RenameFromAbsoluteFilename))
			{
				// Keep the entry visible, but make the unsupported rename topology ineligible.
				Entry.RenameFromAbsoluteFilename.Reset();
			}
		}
		Entry.RecomputeBaseRevertEligibility();
		return AddOrMergeEntry(MoveTemp(Entry), InOutEntryIndicesByPath, OutEntries, OutError);
	}
}

bool FGitChangedAssetsStatus::CaptureSnapshot(const FString& InGitBinary, const FString& InRepositoryRoot, const uint64 InGeneration,
	FGitChangedAssetSnapshot& OutSnapshot, FString& OutError)
{
	OutSnapshot = FGitChangedAssetSnapshot();
	OutError.Reset();
	if (InGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty())
	{
		OutError = TEXT("Git binary path and repository root are required.");
		return false;
	}

	FString RepositoryRoot = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	FPaths::NormalizeDirectoryName(RepositoryRoot);
	FString PinnedHead;
	if (!GitChangedAssetsStatusPrivate::ReadHeadCommitId(InGitBinary, RepositoryRoot, PinnedHead, OutError))
	{
		return false;
	}

	const double StatusStartSeconds = FPlatformTime::Seconds();
	TArray<uint8> StatusOutput;
	if (!GitSourceControlUtils::RunRepositoryStatusPorcelainV2(InGitBinary, RepositoryRoot, StatusOutput, OutError))
	{
		return false;
	}
	const double StatusDurationSeconds = FPlatformTime::Seconds() - StatusStartSeconds;

	TArray<FGitChangedAssetEntry> Entries;
	if (!ParsePorcelainV2(StatusOutput, RepositoryRoot, Entries, OutError))
	{
		return false;
	}

	FString CompletedHead;
	if (!GitChangedAssetsStatusPrivate::ReadHeadCommitId(InGitBinary, RepositoryRoot, CompletedHead, OutError))
	{
		return false;
	}
	if (!PinnedHead.Equals(CompletedHead, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Repository HEAD changed while Changed Assets was refreshing. Refresh again.");
		return false;
	}

	OutSnapshot.GitBinary = InGitBinary;
	OutSnapshot.RepositoryRoot = MoveTemp(RepositoryRoot);
	OutSnapshot.PinnedHead = MoveTemp(PinnedHead);
	OutSnapshot.Generation = InGeneration;
	OutSnapshot.CapturedAtUtc = FDateTime::UtcNow();
	OutSnapshot.StatusDurationSeconds = StatusDurationSeconds;
	OutSnapshot.Entries = MoveTemp(Entries);
	return true;
}

bool FGitChangedAssetsStatus::ParsePorcelainV2(const TArray<uint8>& InOutput, const FString& InRepositoryRoot,
	TArray<FGitChangedAssetEntry>& OutEntries, FString& OutError)
{
	OutEntries.Reset();
	OutError.Reset();
	if (InRepositoryRoot.IsEmpty())
	{
		OutError = TEXT("Repository root is required to parse Changed Assets status.");
		return false;
	}

	int32 Offset = 0;
	TMap<FString, int32> EntryIndicesByPath;
	FString Record;
	while (GitChangedAssetsStatusPrivate::ReadNulToken(InOutput, Offset, Record))
	{
		if (Record.IsEmpty() || Record.StartsWith(TEXT("# "), ESearchCase::CaseSensitive))
		{
			continue;
		}

		const TCHAR RecordType = Record[0];
		if (RecordType == TEXT('!'))
		{
			continue;
		}
		if (RecordType == TEXT('?'))
		{
			if (!Record.StartsWith(TEXT("? "), ESearchCase::CaseSensitive) ||
				!GitChangedAssetsStatusPrivate::AddEntry(InRepositoryRoot, RecordType, Record, Record.Mid(2), FString(), EntryIndicesByPath, OutEntries, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Malformed untracked porcelain-v2 status record.");
				}
				return false;
			}
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
			OutError = FString::Printf(TEXT("Unsupported porcelain-v2 status record type '%c'."), RecordType);
			return false;
		}

		FString CurrentPath;
		if (!GitChangedAssetsStatusPrivate::ExtractPorcelainV2Path(Record, FieldsBeforePath, CurrentPath))
		{
			OutError = TEXT("Malformed porcelain-v2 status path record.");
			return false;
		}

		FString OriginalPath;
		if (RecordType == TEXT('2') && !GitChangedAssetsStatusPrivate::ReadNulToken(InOutput, Offset, OriginalPath))
		{
			OutError = TEXT("Malformed porcelain-v2 rename record without its original path.");
			return false;
		}
		if (!GitChangedAssetsStatusPrivate::AddEntry(InRepositoryRoot, RecordType, Record, CurrentPath, OriginalPath, EntryIndicesByPath, OutEntries, OutError))
		{
			return false;
		}
	}
	return true;
}
