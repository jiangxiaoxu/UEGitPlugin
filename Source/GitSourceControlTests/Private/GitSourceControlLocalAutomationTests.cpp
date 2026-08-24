// Copyright (c) 2026

#include "GitSourceControlUtils.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlLocalAutomationTestsPrivate
{
	FString QuoteGitArgument(const FString& InArgument)
	{
		FString EscapedArgument = InArgument;
		EscapedArgument.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *EscapedArgument);
	}

	FString NormalizeDirectory(const FString& InDirectory)
	{
		FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(InDirectory);
		FPaths::NormalizeDirectoryName(NormalizedDirectory);
		return NormalizedDirectory;
	}

	bool IsSameOrUnderDirectory(const FString& InPath, const FString& InDirectory)
	{
		const FString NormalizedPath = NormalizeDirectory(InPath);
		const FString NormalizedDirectory = NormalizeDirectory(InDirectory);
		return FPaths::IsSamePath(NormalizedPath, NormalizedDirectory) || FPaths::IsUnderDirectory(NormalizedPath, NormalizedDirectory);
	}

	bool RunGitAt(FAutomationTestBase& InTest, const FString& InGitBinary, const FString& InDirectory, const FString& InArguments, FString& OutStandardOutput)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardError;
		const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(InDirectory), *InArguments);
		FPlatformProcess::ExecProcess(*InGitBinary, *CommandLine, &ReturnCode, &OutStandardOutput, &StandardError);
		if (ReturnCode != 0)
		{
			InTest.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
			return false;
		}
		return true;
	}

	bool ParseLfsPointer(const FString& InPointer, FString& OutOid, int64& OutSize)
	{
		OutOid.Reset();
		OutSize = 0;
		TArray<FString> Lines;
		InPointer.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("oid sha256:"), ESearchCase::CaseSensitive))
			{
				OutOid = Line.Mid(11).TrimStartAndEnd();
			}
			else if (Line.StartsWith(TEXT("size "), ESearchCase::CaseSensitive))
			{
				OutSize = FCString::Atoi64(*Line.Mid(5).TrimStartAndEnd());
			}
		}
		return OutOid.Len() == 64 && OutSize >= 0;
	}

	class FGitTestFixture final
	{
	public:
		explicit FGitTestFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FGitTestFixture()
		{
			Cleanup();
		}

		bool Initialize()
		{
			GitBinary = GitSourceControlUtils::FindGitBinaryPath();
			if (GitBinary.IsEmpty())
			{
				Test.AddError(TEXT("Git executable is required for GitSourceControl automation tests."));
				return false;
			}

			const FString TempRoot = NormalizeDirectory(FPlatformProcess::UserTempDir());
			FixtureDirectory = FPaths::Combine(TempRoot, TEXT("GitSourceControlTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FixtureDirectory = NormalizeDirectory(FixtureDirectory);
			if (!IsSafeFixturePath(TempRoot))
			{
				Test.AddError(FString::Printf(TEXT("Refusing to create Git fixture outside a dedicated system-temp directory: %s"), *FixtureDirectory));
				return false;
			}

			if (!IFileManager::Get().MakeDirectory(*FixtureDirectory, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to create Git fixture directory: %s"), *FixtureDirectory));
				return false;
			}

			FString Output;
			if (!RunGit(TEXT("init"), Output) || !RunGit(TEXT("config user.name \"GitSourceControlTests\""), Output) || !RunGit(TEXT("config user.email \"git-source-control-tests@example.invalid\""), Output))
			{
				return false;
			}

			return true;
		}

		bool WriteFile(const FString& InRelativeFilename, const FString& InContents)
		{
			const FString Filename = AbsoluteFilename(InRelativeFilename);
			const FString Directory = FPaths::GetPath(Filename);
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to create fixture file directory: %s"), *Directory));
				return false;
			}

			if (!FFileHelper::SaveStringToFile(InContents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Test.AddError(FString::Printf(TEXT("Failed to write fixture file: %s"), *Filename));
				return false;
			}
			return true;
		}

		bool ReadFile(const FString& InRelativeFilename, FString& OutContents) const
		{
			return FFileHelper::LoadFileToString(OutContents, *AbsoluteFilename(InRelativeFilename));
		}

		bool DeleteFile(const FString& InRelativeFilename)
		{
			const FString Filename = AbsoluteFilename(InRelativeFilename);
			if (!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
			{
				Test.AddError(FString::Printf(TEXT("Failed to delete fixture file: %s"), *Filename));
				return false;
			}
			return true;
		}

		bool RunGit(const FString& InArguments, FString& OutStandardOutput)
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(FixtureDirectory), *InArguments);
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutStandardOutput, &StandardError);
			if (ReturnCode != 0)
			{
				Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
				return false;
			}
			return true;
		}

		bool RunGit(const FString& InArguments)
		{
			FString Output;
			return RunGit(InArguments, Output);
		}

		bool CommitAll(const FString& InMessage)
		{
			return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(InMessage)));
		}

		FString AbsoluteFilename(const FString& InRelativeFilename) const
		{
			return FPaths::Combine(FixtureDirectory, InRelativeFilename);
		}

		FString RelativeFilename(const FString& InAbsoluteFilename) const
		{
			FString RelativeFilename = FPaths::ConvertRelativePathToFull(InAbsoluteFilename);
			FPaths::NormalizeFilename(RelativeFilename);
			FString RepositoryPrefix = FixtureDirectory;
			FPaths::NormalizeDirectoryName(RepositoryPrefix);
			RepositoryPrefix += TEXT("/");
			if (!FPaths::MakePathRelativeTo(RelativeFilename, *RepositoryPrefix))
			{
				Test.AddError(FString::Printf(TEXT("Fixture path is outside the Git repository: %s"), *InAbsoluteFilename));
				return FString();
			}
			FPaths::NormalizeFilename(RelativeFilename);
			return RelativeFilename;
		}

		const FString& GetDirectory() const
		{
			return FixtureDirectory;
		}

		const FString& GetGitBinary() const
		{
			return GitBinary;
		}

	private:
		bool IsSafeFixturePath(const FString& InTempRoot) const
		{
			const FString RequiredPrefix = FPaths::Combine(InTempRoot, TEXT("GitSourceControlTests"));
			if (!IsSameOrUnderDirectory(FixtureDirectory, RequiredPrefix) || IsSameOrUnderDirectory(FixtureDirectory, FPaths::ProjectDir()))
			{
				return false;
			}

			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (IsSameOrUnderDirectory(FixtureDirectory, Plugin->GetBaseDir()))
				{
					return false;
				}
			}
			return true;
		}

		void Cleanup()
		{
			if (FixtureDirectory.IsEmpty())
			{
				return;
			}

			const FString TempRoot = NormalizeDirectory(FPlatformProcess::UserTempDir());
			if (!IsSafeFixturePath(TempRoot))
			{
				Test.AddError(FString::Printf(TEXT("Refusing to delete unsafe Git fixture directory: %s"), *FixtureDirectory));
				return;
			}

			if (IFileManager::Get().DirectoryExists(*FixtureDirectory) && !IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to delete Git fixture directory: %s"), *FixtureDirectory));
			}
			FixtureDirectory.Reset();
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString FixtureDirectory;
	};

	bool CreateCommittedFixture(FGitTestFixture& InFixture)
	{
		return InFixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("initial\n"))
			&& InFixture.WriteFile(TEXT("Content/Unicode 空格.txt"), TEXT("unicode\n"))
			&& InFixture.WriteFile(TEXT("Content/Rename Old.txt"), TEXT("rename\n"))
			&& InFixture.WriteFile(TEXT(".gitignore"), TEXT("Ignored/\n"))
			&& InFixture.CommitAll(TEXT("Initial fixture"));
	}

	const FGitSourceControlState* FindState(const TMap<FString, FGitSourceControlState>& InStates, const FString& InAbsoluteFilename)
	{
		if (const FGitSourceControlState* State = InStates.Find(InAbsoluteFilename))
		{
			return State;
		}

		FString NormalizedFilename = FPaths::ConvertRelativePathToFull(InAbsoluteFilename);
		FPaths::NormalizeFilename(NormalizedFilename);
		for (const TPair<FString, FGitSourceControlState>& Pair : InStates)
		{
			FString NormalizedKey = FPaths::ConvertRelativePathToFull(Pair.Key);
			FPaths::NormalizeFilename(NormalizedKey);
			if (FPaths::IsSamePath(NormalizedFilename, NormalizedKey))
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}

	bool QuerySingleState(FAutomationTestBase& InTest, const FGitTestFixture& InFixture, const FString& InAbsoluteFilename, FGitSourceControlState& OutState)
	{
		TArray<FString> Errors;
		TMap<FString, FGitSourceControlState> States;
		if (!GitSourceControlUtils::RunUpdateStatus(InFixture.GetGitBinary(), InFixture.GetDirectory(), false, { InAbsoluteFilename }, Errors, States))
		{
			InTest.AddError(FString::Printf(TEXT("RunUpdateStatus failed for %s: %s"), *InAbsoluteFilename, *FString::Join(Errors, TEXT("\n"))));
			return false;
		}

		const FGitSourceControlState* State = FindState(States, InAbsoluteFilename);
		if (State == nullptr)
		{
			InTest.AddError(FString::Printf(TEXT("RunUpdateStatus returned no state for %s"), *InAbsoluteFilename));
			return false;
		}
		OutState = *State;
		return true;
	}

	bool AssertState(FAutomationTestBase& InTest, const FGitSourceControlState& InState, const EFileState::Type InExpectedFileState, const ETreeState::Type InExpectedTreeState, const FString& InLabel)
	{
		const bool bFileStateMatches = InTest.TestEqual(*FString::Printf(TEXT("%s file state"), *InLabel), static_cast<int32>(InState.State.FileState), static_cast<int32>(InExpectedFileState));
		const bool bTreeStateMatches = InTest.TestEqual(*FString::Printf(TEXT("%s tree state"), *InLabel), static_cast<int32>(InState.State.TreeState), static_cast<int32>(InExpectedTreeState));
		return bFileStateMatches && bTreeStateMatches;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLocalStatusAutomationTest,
	"Cthulhu.GitSourceControl.Local.Status",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLocalStatusAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture))
	{
		return false;
	}

	const FString TrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt"));
	FGitSourceControlState State(TrackedFilename);
	if (!QuerySingleState(*this, Fixture, TrackedFilename, State) || !AssertState(*this, State, EFileState::Unknown, ETreeState::Unmodified, TEXT("Clean tracked file")))
	{
		return false;
	}

	if (!Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("modified\n")) || !QuerySingleState(*this, Fixture, TrackedFilename, State) || !AssertState(*this, State, EFileState::Modified, ETreeState::Working, TEXT("Modified tracked file")))
	{
		return false;
	}

	if (!Fixture.RunGit(FString::Printf(TEXT("add -- %s"), *GitSourceControlLocalAutomationTestsPrivate::QuoteGitArgument(Fixture.RelativeFilename(TrackedFilename)))) || !QuerySingleState(*this, Fixture, TrackedFilename, State) || !AssertState(*this, State, EFileState::Modified, ETreeState::Staged, TEXT("Staged tracked file")))
	{
		return false;
	}

	if (!Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("staged and working\n")) || !QuerySingleState(*this, Fixture, TrackedFilename, State) || !AssertState(*this, State, EFileState::Modified, ETreeState::Working, TEXT("Staged and working tracked file")))
	{
		return false;
	}

	if (!Fixture.DeleteFile(TEXT("Content/Tracked.txt")) || !QuerySingleState(*this, Fixture, TrackedFilename, State) || !AssertState(*this, State, EFileState::Deleted, ETreeState::Working, TEXT("Deleted tracked file")))
	{
		return false;
	}

	const FString UnicodeFilename = Fixture.AbsoluteFilename(TEXT("Content/Unicode 空格.txt"));
	if (!Fixture.WriteFile(TEXT("Content/Unicode 空格.txt"), TEXT("unicode modified\n")) || !QuerySingleState(*this, Fixture, UnicodeFilename, State) || !AssertState(*this, State, EFileState::Modified, ETreeState::Working, TEXT("Unicode path")))
	{
		return false;
	}

	const FString UntrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/New Asset.txt"));
	if (!Fixture.WriteFile(TEXT("Content/New Asset.txt"), TEXT("untracked\n")) || !QuerySingleState(*this, Fixture, UntrackedFilename, State) || !AssertState(*this, State, EFileState::Unknown, ETreeState::Untracked, TEXT("Untracked file")))
	{
		return false;
	}

	const FString IgnoredFilename = Fixture.AbsoluteFilename(TEXT("Ignored/Discard.txt"));
	if (!Fixture.WriteFile(TEXT("Ignored/Discard.txt"), TEXT("ignored\n")) || !QuerySingleState(*this, Fixture, IgnoredFilename, State) || !AssertState(*this, State, EFileState::Unknown, ETreeState::Ignored, TEXT("Ignored file")))
	{
		return false;
	}

	TArray<FString> Errors;
	TMap<FString, FGitSourceControlState> DirectoryStates;
	TestFalse(TEXT("Directory-wide status is rejected"), GitSourceControlUtils::RunUpdateStatus(Fixture.GetGitBinary(), Fixture.GetDirectory(), false, { Fixture.GetDirectory() }, Errors, DirectoryStates));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlRenamePairAutomationTest,
	"Cthulhu.GitSourceControl.Local.RenamePair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlRenamePairAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture))
	{
		return false;
	}

	const FString OldFilename = Fixture.AbsoluteFilename(TEXT("Content/Rename Old.txt"));
	const FString NewFilename = Fixture.AbsoluteFilename(TEXT("Content/Rename New.txt"));
	if (!Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(Fixture.RelativeFilename(OldFilename)), *QuoteGitArgument(Fixture.RelativeFilename(NewFilename)))))
	{
		return false;
	}

	TArray<FString> ExpandedFiles;
	TArray<FGitRenamePair> RenamePairs;
	TArray<FString> Errors;
	if (!TestTrue(TEXT("Rename pair expands from the new path"), GitSourceControlUtils::ExpandSelectedPathsWithRenamePairs(Fixture.GetGitBinary(), Fixture.GetDirectory(), { NewFilename }, ExpandedFiles, RenamePairs, Errors)))
	{
		AddError(FString::Join(Errors, TEXT("\n")));
		return false;
	}

	TestEqual(TEXT("Exactly one rename pair is returned"), RenamePairs.Num(), 1);
	if (RenamePairs.Num() == 1)
	{
		TestTrue(TEXT("Rename pair contains old file"), FPaths::IsSamePath(RenamePairs[0].OldPath, OldFilename));
		TestTrue(TEXT("Rename pair contains new file"), FPaths::IsSamePath(RenamePairs[0].NewPath, NewFilename));
	}
	TestEqual(TEXT("Both rename paths are expanded"), ExpandedFiles.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlHistoryAndBlobAutomationTest,
	"Cthulhu.GitSourceControl.Local.HistoryAndBlob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlHistoryAndBlobAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture))
	{
		return false;
	}

	const FString RelativeFilename = TEXT("Content/Tracked.txt");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	if (!Fixture.WriteFile(RelativeFilename, TEXT("second revision\n")) || !Fixture.CommitAll(TEXT("Second fixture revision")))
	{
		return false;
	}

	TArray<FString> Errors;
	TGitSourceControlHistory History;
	if (!TestTrue(TEXT("History query succeeds"), GitSourceControlUtils::RunGetHistory(Fixture.GetGitBinary(), Fixture.GetDirectory(), Filename, false, Errors, History)))
	{
		AddError(FString::Join(Errors, TEXT("\n")));
		return false;
	}
	TestTrue(TEXT("History includes both committed revisions"), History.Num() >= 2);
	if (History.Num() == 0)
	{
		return false;
	}
	TestTrue(TEXT("Current revision has a full commit id"), History[0]->CommitId.Len() >= 40);
	TestEqual(TEXT("Current revision records the repository root"), History[0]->PathToRepoRoot, Fixture.GetDirectory());

	const FString ExportFilename = Fixture.AbsoluteFilename(TEXT("Exported/Tracked.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExportFilename), true);
	FString ExportError;
	if (!TestTrue(TEXT("HEAD blob export succeeds"), GitSourceControlUtils::DumpRevisionBlobToFile(Fixture.GetGitBinary(), Fixture.GetDirectory(), TEXT("HEAD:Content/Tracked.txt"), ExportFilename, ExportError)))
	{
		AddError(ExportError);
		return false;
	}

	FString ExportedContents;
	TestTrue(TEXT("Exported blob can be read"), FFileHelper::LoadFileToString(ExportedContents, *ExportFilename));
	TestEqual(TEXT("Exported blob contains HEAD content"), ExportedContents, FString(TEXT("second revision\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlCapabilitiesAutomationTest,
	"Cthulhu.GitSourceControl.Local.Capabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlCapabilitiesAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}

	FString Error;
	if (!TestTrue(TEXT("Local Git capabilities are available"), GitSourceControlUtils::CheckLocalGitCapabilities(Fixture.GetGitBinary(), Fixture.GetDirectory(), Error)))
	{
		AddError(Error);
		return false;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsObjectVerificationAutomationTest,
	"Cthulhu.GitSourceControl.Local.LfsObjectVerification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsObjectVerificationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;

	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty()))
	{
		return false;
	}

	const FString TempRoot = NormalizeDirectory(FPlatformProcess::UserTempDir());
	const FString FixtureDirectory = NormalizeDirectory(FPaths::Combine(TempRoot, TEXT("GitSourceControlLfsTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	bool bSafeFixturePath = !IsSameOrUnderDirectory(FixtureDirectory, FPaths::ProjectDir());
	for (const TSharedRef<IPlugin>& DiscoveredPlugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		bSafeFixturePath = bSafeFixturePath && !IsSameOrUnderDirectory(FixtureDirectory, DiscoveredPlugin->GetBaseDir());
	}
	if (!TestTrue(TEXT("LFS fixture path is outside the project and plugins"), bSafeFixturePath))
	{
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*FixtureDirectory, true))
	{
		AddError(FString::Printf(TEXT("Failed to create LFS fixture directory: %s"), *FixtureDirectory));
		return false;
	}

	const FString ObjectFilename = FPaths::Combine(FixtureDirectory, TEXT("object.bin"));
	const FString ValidContents = TEXT("valid local LFS payload\n");
	const FString CorruptContents = TEXT("valid local LFS payloxd\n");
	bool bSucceeded = FFileHelper::SaveStringToFile(ValidContents, *ObjectFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString PointerOutput;
	int32 ReturnCode = INDEX_NONE;
	FString PointerError;
	if (bSucceeded)
	{
		const FString CommandLine = FString::Printf(TEXT("lfs pointer --file=%s"), *QuoteGitArgument(ObjectFilename));
		FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &PointerOutput, &PointerError);
		bSucceeded = ReturnCode == 0;
	}

	FString ExpectedOid;
	int64 ExpectedSize = -1;
	if (bSucceeded)
	{
		TArray<FString> Lines;
		PointerOutput.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("oid sha256:"), ESearchCase::CaseSensitive))
			{
				ExpectedOid = Line.Mid(11).TrimStartAndEnd();
			}
			else if (Line.StartsWith(TEXT("size "), ESearchCase::CaseSensitive))
			{
				ExpectedSize = FCString::Atoi64(*Line.Mid(5).TrimStartAndEnd());
			}
		}
		bSucceeded = ExpectedOid.Len() == 64 && ExpectedSize == IFileManager::Get().FileSize(*ObjectFilename);
	}
	if (!TestTrue(TEXT("Pointer metadata can be read"), bSucceeded))
	{
		AddError(PointerError);
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
		return false;
	}

	FString VerificationError;
	TestTrue(TEXT("Valid local LFS object passes OID and size verification"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, TempRoot, ObjectFilename, ExpectedOid, ExpectedSize, VerificationError));
	if (!FFileHelper::SaveStringToFile(CorruptContents, *ObjectFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(TEXT("Failed to write same-size corrupt LFS fixture."));
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
		return false;
	}
	VerificationError.Reset();
	TestFalse(TEXT("Same-size corrupt local LFS object is rejected"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, TempRoot, ObjectFilename, ExpectedOid, ExpectedSize, VerificationError));
	FString CorruptFileContents;
	TestTrue(TEXT("Corrupt verification leaves the object as ordinary worktree bytes"), FFileHelper::LoadFileToString(CorruptFileContents, *ObjectFilename) && !CorruptFileContents.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
	IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsRevisionFetchAutomationTest,
	"Cthulhu.GitSourceControl.Local.LfsRevisionFetch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsRevisionFetchAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty()))
	{
		return false;
	}

	int32 LfsVersionExitCode = INDEX_NONE;
	FString LfsVersionOutput;
	FString LfsVersionError;
	FPlatformProcess::ExecProcess(*GitBinary, TEXT("lfs version"), &LfsVersionExitCode, &LfsVersionOutput, &LfsVersionError);
	if (LfsVersionExitCode != 0)
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping the local LFS revision fetch fixture."));
		return true;
	}

	const FString TempRoot = NormalizeDirectory(FPlatformProcess::UserTempDir());
	const FString FixtureRoot = NormalizeDirectory(FPaths::Combine(TempRoot, TEXT("GitSourceControlLfsFetchTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("LFS fetch fixture is outside project and plugins"), !IsSameOrUnderDirectory(FixtureRoot, FPaths::ProjectDir())))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	};
	if (!IFileManager::Get().MakeDirectory(*FixtureRoot, true))
	{
		AddError(TEXT("Could not create the local LFS fetch fixture root."));
		return false;
	}

	const FString OriginDirectory = FPaths::Combine(FixtureRoot, TEXT("origin.git"));
	const FString SourceDirectory = FPaths::Combine(FixtureRoot, TEXT("source"));
	const FString CloneDirectory = FPaths::Combine(FixtureRoot, TEXT("clone"));
	FString Output;
	if (!RunGitAt(*this, GitBinary, FixtureRoot, FString::Printf(TEXT("init --bare %s"), *QuoteGitArgument(OriginDirectory)), Output) ||
		!RunGitAt(*this, GitBinary, FixtureRoot, FString::Printf(TEXT("init %s"), *QuoteGitArgument(SourceDirectory)), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("config user.name \"GitSourceControlTests\""), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("config user.email \"git-source-control-tests@example.invalid\""), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("lfs install --local"), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("lfs track \"*.bin\""), Output))
	{
		return false;
	}

	const FString SourcePayload = FPaths::Combine(SourceDirectory, TEXT("Content/LfsPayload.bin"));
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePayload), true) ||
		!FFileHelper::SaveStringToFile(TEXT("LFS revision payload\n"), *SourcePayload, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("add .gitattributes Content/LfsPayload.bin"), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("commit --no-gpg-sign -m \"LFS fetch fixture\""), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(OriginDirectory)), Output) ||
		!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("push origin HEAD"), Output))
	{
		return false;
	}

	FString BranchName;
	if (!RunGitAt(*this, GitBinary, SourceDirectory, TEXT("branch --show-current"), BranchName))
	{
		return false;
	}
	BranchName.TrimStartAndEndInline();
	if (BranchName.IsEmpty() || !RunGitAt(*this, GitBinary, OriginDirectory, FString::Printf(TEXT("symbolic-ref HEAD refs/heads/%s"), *BranchName), Output) ||
		!RunGitAt(*this, GitBinary, FixtureRoot, FString::Printf(TEXT("clone %s %s"), *QuoteGitArgument(OriginDirectory), *QuoteGitArgument(CloneDirectory)), Output))
	{
		return false;
	}

	FString CommitId;
	FString PointerText;
	if (!RunGitAt(*this, GitBinary, CloneDirectory, TEXT("rev-parse HEAD"), CommitId) ||
		!RunGitAt(*this, GitBinary, CloneDirectory, TEXT("show HEAD:Content/LfsPayload.bin"), PointerText))
	{
		return false;
	}
	CommitId.TrimStartAndEndInline();
	FString ObjectOid;
	int64 ObjectSize = -1;
	if (!TestTrue(TEXT("Fixture exposes valid LFS pointer metadata"), ParseLfsPointer(PointerText, ObjectOid, ObjectSize)))
	{
		return false;
	}
	const FString ObjectFilename = FPaths::Combine(CloneDirectory, TEXT(".git/lfs/objects"), ObjectOid.Left(2), ObjectOid.Mid(2, 2), ObjectOid);
	if (!TestTrue(TEXT("Clone initially materializes its LFS object"), IFileManager::Get().FileExists(*ObjectFilename)) ||
		!TestTrue(TEXT("Fixture removes local LFS object before on-demand fetch"), IFileManager::Get().Delete(*ObjectFilename, false, true)))
	{
		return false;
	}

	FString FetchError;
	if (!TestTrue(TEXT("Precise historical LFS fetch succeeds from local bare remote"),
		GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, CloneDirectory, CommitId, TEXT("Content/LfsPayload.bin"), FetchError)))
	{
		AddError(FetchError);
		return false;
	}
	TestTrue(TEXT("Fetched LFS object passes OID and size verification"),
		GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, CloneDirectory, ObjectFilename, ObjectOid, ObjectSize, FetchError));

	if (!TestTrue(TEXT("Fixture removes fetched object before cancellation"), IFileManager::Get().Delete(*ObjectFilename, false, true)))
	{
		return false;
	}
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
		MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	CancellationContext->Cancel();
	GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
	FetchError.Reset();
	TestFalse(TEXT("Cancelled LFS download performs no fetch"),
		GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, CloneDirectory, CommitId, TEXT("Content/LfsPayload.bin"), FetchError));
	TestTrue(TEXT("Cancelled LFS download keeps object missing"), !IFileManager::Get().FileExists(*ObjectFilename));
	TestTrue(TEXT("Cancelled LFS download reports cancellation"), FetchError.Contains(TEXT("cancel"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsLiteralPathValidationAutomationTest,
	"Cthulhu.GitSourceControl.Local.LfsLiteralPathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsLiteralPathValidationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty()))
	{
		return false;
	}

	const TArray<FString> InvalidPaths =
	{
		TEXT("Content/*.uasset"),
		TEXT("Content/Asset,Other.uasset"),
		TEXT("Content/[Asset].uasset"),
		TEXT("Content/!Asset.uasset"),
		TEXT("Content/#Asset.uasset"),
		TEXT("Content\\Asset.uasset"),
		TEXT("Content/../Asset.uasset"),
		TEXT("Content//Asset.uasset")
	};
	for (const FString& InvalidPath : InvalidPaths)
	{
		FString Error;
		const bool bFetched = GitSourceControlUtils::FetchLfsContentForRevision(
			GitBinary,
			FPaths::ProjectDir(),
			TEXT("0123456789012345678901234567890123456789"),
			InvalidPath,
			Error);
		TestFalse(FString::Printf(TEXT("Git LFS pattern is rejected before process launch: %s"), *InvalidPath), bFetched);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsNonGameThreadCancellationAutomationTest,
	"Cthulhu.GitSourceControl.Local.LfsNonGameThreadCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsNonGameThreadCancellationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty()))
	{
		return false;
	}

	TFuture<bool> Future = Async(EAsyncExecution::ThreadPool, [GitBinary]()
	{
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
			MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		CancellationContext->Cancel();
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
		FString Error;
		return !GitSourceControlUtils::FetchLfsContentForRevision(
			GitBinary,
			FPaths::ProjectDir(),
			TEXT("0123456789012345678901234567890123456789"),
			TEXT("Content/LfsPayload.bin"),
			Error);
	});
	TestTrue(TEXT("Pre-cancelled non-game-thread LFS request completes promptly"), Future.WaitFor(FTimespan::FromSeconds(2.0)));
	if (Future.IsReady())
	{
		TestTrue(TEXT("Pre-cancelled non-game-thread LFS request is rejected"), Future.Get());
	}
	return true;
}

#endif

IMPLEMENT_MODULE(FDefaultModuleImpl, GitSourceControlTests)
