// Copyright (c) 2026

#include "GitChangedAssetsStatus.h"
#include "GitSourceControlUtils.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitChangedAssetsStatusAutomationTestsPrivate
{
	FString QuoteGitArgument(const FString& InArgument)
	{
		FString Escaped = InArgument;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	void AppendNulUtf8(TArray<uint8>& InOutBuffer, const FString& InToken)
	{
		FTCHARToUTF8 Utf8(*InToken);
		InOutBuffer.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		InOutBuffer.Add(0);
	}

	class FFixture final
	{
	public:
		explicit FFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FFixture()
		{
			if (!Root.IsEmpty() && IsSafePath() && IFileManager::Get().DirectoryExists(*Root))
			{
				IFileManager::Get().DeleteDirectory(*Root, false, true);
			}
		}

		bool Initialize()
		{
			GitBinary = GitSourceControlUtils::FindGitBinaryPath();
			if (GitBinary.IsEmpty())
			{
				Test.AddError(TEXT("Git executable is required for Changed Assets automation tests."));
				return false;
			}
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetsStatusTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			FPaths::NormalizeDirectoryName(Root);
			if (!IsSafePath() || !IFileManager::Get().MakeDirectory(*Root, true))
			{
				Test.AddError(FString::Printf(TEXT("Could not create Changed Assets fixture: %s"), *Root));
				return false;
			}
			return RunGit(TEXT("init")) && RunGit(TEXT("config user.name \"GitChangedAssetsTests\"")) && RunGit(TEXT("config user.email \"git-changed-assets-tests@example.invalid\""));
		}

		bool WriteFile(const FString& InRelativeFilename, const FString& InContents) const
		{
			const FString Filename = AbsoluteFilename(InRelativeFilename);
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)
				&& FFileHelper::SaveStringToFile(InContents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool DeleteFile(const FString& InRelativeFilename) const
		{
			return IFileManager::Get().Delete(*AbsoluteFilename(InRelativeFilename), false, true, true);
		}

		bool RunGit(const FString& InArguments) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardOutput;
			FString StandardError;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *InArguments);
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &StandardOutput, &StandardError);
			if (ReturnCode == 0)
			{
				return true;
			}
			Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
			return false;
		}

		bool CommitAll(const FString& InMessage) const
		{
			return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(InMessage)));
		}

		FString AbsoluteFilename(const FString& InRelativeFilename) const { return FPaths::Combine(Root, InRelativeFilename); }
		const FString& GetGitBinary() const { return GitBinary; }
		const FString& GetRoot() const { return Root; }

	private:
		bool IsSafePath() const
		{
			FString Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetsStatusTests")));
			FPaths::NormalizeDirectoryName(Parent);
			return FPaths::IsUnderDirectory(Root, Parent) && !FPaths::IsUnderDirectory(Root, FPaths::ProjectDir());
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString Root;
	};

	const FGitChangedAssetEntry* FindByPath(const TArray<FGitChangedAssetEntry>& InEntries, const FString& InRepositoryRelativePath)
	{
		return InEntries.FindByPredicate([&InRepositoryRelativePath](const FGitChangedAssetEntry& Entry)
		{
			return Entry.RepositoryRelativePath == InRepositoryRelativePath;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsStatusParserAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.StatusParser", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsStatusParserAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetsStatusAutomationTestsPrivate;

	TArray<uint8> Output;
	AppendNulUtf8(Output, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/Modified.uasset"));
	AppendNulUtf8(Output, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/Renamed New.uasset"));
	AppendNulUtf8(Output, TEXT("Content/Renamed Old.uasset"));
	AppendNulUtf8(Output, TEXT("? Content/Untracked Asset.uasset"));
	AppendNulUtf8(Output, TEXT("u UU N... 100644 100644 100644 100644 1111111 2222222 3333333 Content/Conflict.uasset"));
	AppendNulUtf8(Output, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/NotAnAsset.txt"));

	TArray<FGitChangedAssetEntry> Entries;
	FString Error;
	if (!TestTrue(TEXT("NUL-safe porcelain-v2 parser accepts mixed records"), FGitChangedAssetsStatus::ParsePorcelainV2(Output, FPaths::ProjectDir(), Entries, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Only .uasset records become Changed Assets entries"), Entries.Num(), 4);
	const FGitChangedAssetEntry* Modified = FindByPath(Entries, TEXT("Content/Modified.uasset"));
	const FGitChangedAssetEntry* Renamed = FindByPath(Entries, TEXT("Content/Renamed New.uasset"));
	const FGitChangedAssetEntry* Untracked = FindByPath(Entries, TEXT("Content/Untracked Asset.uasset"));
	const FGitChangedAssetEntry* Conflict = FindByPath(Entries, TEXT("Content/Conflict.uasset"));
	if (!TestNotNull(TEXT("Modified asset exists"), Modified) || !TestNotNull(TEXT("Renamed asset exists"), Renamed)
		|| !TestNotNull(TEXT("Untracked asset exists"), Untracked) || !TestNotNull(TEXT("Conflicted asset exists"), Conflict))
	{
		return false;
	}
	TestEqual(TEXT("Modified state is aggregated"), Modified->State, EGitChangedAssetState::Modified);
	TestEqual(TEXT("Rename keeps the old NUL path"), Renamed->RenameFromRepositoryRelativePath, FString(TEXT("Content/Renamed Old.uasset")));
	TestEqual(TEXT("Rename has its raw index state"), Renamed->IndexStatus, TEXT('R'));
	TestEqual(TEXT("Untracked state is aggregated"), Untracked->State, EGitChangedAssetState::Untracked);
	TestEqual(TEXT("Conflict state is aggregated"), Conflict->State, EGitChangedAssetState::Conflicted);
	TestTrue(TEXT("Conflict is not base-revertable"), !Conflict->bBaseRevertEligible);

	TArray<uint8> ReplacementOutput;
	AppendNulUtf8(ReplacementOutput, TEXT("? Content/Replaced.uasset"));
	AppendNulUtf8(ReplacementOutput, TEXT("1 D. N... 100644 000000 000000 1111111 0000000 Content/Replaced.uasset"));
	TArray<FGitChangedAssetEntry> ReplacementEntries;
	Error.Reset();
	if (!TestTrue(TEXT("Tracked deletion and untracked replacement aggregate regardless of record order"),
		FGitChangedAssetsStatus::ParsePorcelainV2(ReplacementOutput, FPaths::ProjectDir(), ReplacementEntries, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("Parser emits one normalized replacement path"), ReplacementEntries.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Replacement keeps tracked deletion state"), ReplacementEntries[0].State, EGitChangedAssetState::Deleted);
	TestEqual(TEXT("Replacement keeps tracked XY"), ReplacementEntries[0].IndexStatus, TEXT('D'));
	TestTrue(TEXT("Replacement tracks its untracked worktree topology"), ReplacementEntries[0].bHasUntrackedReplacement);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsSnapshotAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.Snapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsSnapshotAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetsStatusAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Modified.uasset"), TEXT("head modified\n"))
		|| !Fixture.WriteFile(TEXT("Content/Deleted.uasset"), TEXT("head deleted\n"))
		|| !Fixture.WriteFile(TEXT("Content/Renamed Old.uasset"), TEXT("head renamed\n"))
		|| !Fixture.CommitAll(TEXT("Initial Changed Assets snapshot fixture"))
		|| !Fixture.WriteFile(TEXT("Content/Modified.uasset"), TEXT("working modified\n"))
		|| !Fixture.DeleteFile(TEXT("Content/Deleted.uasset"))
		|| !Fixture.RunGit(TEXT("mv -- \"Content/Renamed Old.uasset\" \"Content/Renamed New.uasset\""))
		|| !Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("staged added\n"))
		|| !Fixture.RunGit(TEXT("add -- Content/Added.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Untracked Asset.uasset"), TEXT("untracked\n"))
		|| !Fixture.WriteFile(TEXT("Content/NotAnAsset.txt"), TEXT("ignored by Changed Assets\n")))
	{
		return false;
	}

	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FGitChangedAssetSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Repository-wide Changed Assets snapshot succeeds"), FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 17, Snapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Snapshot preserves requested generation"), Snapshot.Generation, static_cast<uint64>(17));
	TestTrue(TEXT("Snapshot pins HEAD"), !Snapshot.PinnedHead.IsEmpty());
	TestTrue(TEXT("Repository-wide status process is bounded by two HEAD checks"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount() == static_cast<uint64>(3));
	TestEqual(TEXT("Snapshot only contains changed .uasset files"), Snapshot.Entries.Num(), 5);
	const FGitChangedAssetEntry* Modified = FindByPath(Snapshot.Entries, TEXT("Content/Modified.uasset"));
	const FGitChangedAssetEntry* Deleted = FindByPath(Snapshot.Entries, TEXT("Content/Deleted.uasset"));
	const FGitChangedAssetEntry* Renamed = FindByPath(Snapshot.Entries, TEXT("Content/Renamed New.uasset"));
	const FGitChangedAssetEntry* Added = FindByPath(Snapshot.Entries, TEXT("Content/Added.uasset"));
	const FGitChangedAssetEntry* Untracked = FindByPath(Snapshot.Entries, TEXT("Content/Untracked Asset.uasset"));
	if (!TestNotNull(TEXT("Modified entry is present"), Modified) || !TestNotNull(TEXT("Deleted entry is present"), Deleted)
		|| !TestNotNull(TEXT("Renamed entry is present"), Renamed) || !TestNotNull(TEXT("Added entry is present"), Added)
		|| !TestNotNull(TEXT("Untracked entry is present"), Untracked))
	{
		return false;
	}
	TestEqual(TEXT("Modified entry state"), Modified->State, EGitChangedAssetState::Modified);
	TestEqual(TEXT("Deleted entry state"), Deleted->State, EGitChangedAssetState::Deleted);
	TestEqual(TEXT("Rename entry state"), Renamed->State, EGitChangedAssetState::Renamed);
	TestEqual(TEXT("Rename old path"), Renamed->RenameFromRepositoryRelativePath, FString(TEXT("Content/Renamed Old.uasset")));
	TestEqual(TEXT("Added entry state"), Added->State, EGitChangedAssetState::Added);
	return TestEqual(TEXT("Untracked entry state"), Untracked->State, EGitChangedAssetState::Untracked);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsTrackedReplacementAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.TrackedReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsTrackedReplacementAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetsStatusAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Replaced.uasset"), TEXT("HEAD asset bytes\n"))
		|| !Fixture.CommitAll(TEXT("Initial tracked replacement fixture"))
		|| !Fixture.RunGit(TEXT("rm -- Content/Replaced.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Replaced.uasset"), TEXT("new untracked replacement bytes\n")))
	{
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Snapshot handles staged deletion with an untracked replacement"),
		FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("Same normalized .uasset path is represented by one entry"), Snapshot.Entries.Num(), 1))
	{
		return false;
	}

	const FGitChangedAssetEntry& Entry = Snapshot.Entries[0];
	TestEqual(TEXT("Tracked record wins aggregate state"), Entry.State, EGitChangedAssetState::Deleted);
	TestEqual(TEXT("Tracked index deletion XY is retained"), Entry.IndexStatus, TEXT('D'));
	TestEqual(TEXT("Tracked worktree XY is retained"), Entry.WorktreeStatus, TEXT('.'));
	TestTrue(TEXT("Untracked replacement topology is retained"), Entry.bHasUntrackedReplacement);
	TestTrue(TEXT("Tracked deletion remains base-revertable"), Entry.bBaseRevertEligible);
	TestTrue(TEXT("Metadata gate remains the only initial Revert blocker"), !Entry.bCanRevert);

	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	TArray<uint8> RecheckOutput;
	if (!TestTrue(TEXT("Exact-path status recheck preserves the replacement topology"),
		GitSourceControlUtils::RunPathsStatusPorcelainV2(Fixture.GetGitBinary(), Fixture.GetRoot(), { Fixture.AbsoluteFilename(TEXT("Content/Replaced.uasset")) }, RecheckOutput, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Exact-path status recheck starts one Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(1));
	TArray<FGitChangedAssetEntry> RecheckEntries;
	Error.Reset();
	if (!TestTrue(TEXT("Exact-path status recheck is parsed by the shared aggregate parser"),
		FGitChangedAssetsStatus::ParsePorcelainV2(RecheckOutput, Fixture.GetRoot(), RecheckEntries, Error)))
	{
		AddError(Error);
		return false;
	}
	return TestTrue(TEXT("Exact-path recheck retains the tracked deletion baseline"), RecheckEntries.Num() == 1
		&& RecheckEntries[0].State == EGitChangedAssetState::Deleted && RecheckEntries[0].IndexStatus == TEXT('D')
		&& RecheckEntries[0].bHasUntrackedReplacement);
}

#endif
