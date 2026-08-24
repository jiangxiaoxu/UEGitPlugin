// Copyright (c) 2026

#include "GitSourceControlAssetOperations.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlAssetOperationsAutomationTestsPrivate
{
	FString QuoteGitArgument(const FString& InArgument)
	{
		FString EscapedArgument = InArgument;
		EscapedArgument.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *EscapedArgument);
	}

	FString NormalizeDirectory(const FString& InDirectory)
	{
		FString Result = FPaths::ConvertRelativePathToFull(InDirectory);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	bool IsSameOrUnderDirectory(const FString& InPath, const FString& InDirectory)
	{
		const FString Path = NormalizeDirectory(InPath);
		const FString Directory = NormalizeDirectory(InDirectory);
		return FPaths::IsSamePath(Path, Directory) || FPaths::IsUnderDirectory(Path, Directory);
	}

	class FGitAssetOperationFixture final
	{
	public:
		explicit FGitAssetOperationFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FGitAssetOperationFixture()
		{
			Cleanup();
		}

		bool Initialize()
		{
			GitBinary = GitSourceControlUtils::FindGitBinaryPath();
			if (GitBinary.IsEmpty())
			{
				Test.AddError(TEXT("Git executable is required for Git asset-operation automation tests."));
				return false;
			}

			const FString TempRoot = NormalizeDirectory(FPlatformProcess::UserTempDir());
			RepositoryRoot = NormalizeDirectory(FPaths::Combine(TempRoot, TEXT("GitSourceControlAssetOperationTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IsSafeFixturePath(TempRoot))
			{
				Test.AddError(FString::Printf(TEXT("Refusing to create Git asset-operation fixture outside system temp: %s"), *RepositoryRoot));
				return false;
			}
			if (!IFileManager::Get().MakeDirectory(*RepositoryRoot, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to create Git asset-operation fixture directory: %s"), *RepositoryRoot));
				return false;
			}

			return RunGit(TEXT("init"))
				&& RunGit(TEXT("config user.name \"GitSourceControlTests\""))
				&& RunGit(TEXT("config user.email \"git-source-control-tests@example.invalid\""));
		}

		bool WriteFile(const FString& InRelativeFilename, const FString& InContents)
		{
			const FString Filename = AbsoluteFilename(InRelativeFilename);
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to create fixture file directory for %s"), *Filename));
				return false;
			}
			if (!FFileHelper::SaveStringToFile(InContents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Test.AddError(FString::Printf(TEXT("Failed to write fixture file %s"), *Filename));
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
				Test.AddError(FString::Printf(TEXT("Failed to delete fixture file %s"), *Filename));
				return false;
			}
			return true;
		}

		bool MoveFile(const FString& InSourceRelativeFilename, const FString& InDestinationRelativeFilename)
		{
			const FString SourceFilename = AbsoluteFilename(InSourceRelativeFilename);
			const FString DestinationFilename = AbsoluteFilename(InDestinationRelativeFilename);
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationFilename), true) ||
				!IFileManager::Get().Move(*DestinationFilename, *SourceFilename, true, true, false, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to move fixture file from %s to %s"), *SourceFilename, *DestinationFilename));
				return false;
			}
			return true;
		}

		bool CommitAll(const FString& InMessage)
		{
			return RunGit(TEXT("add --all"))
				&& RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(InMessage)));
		}

		bool RunGit(const FString& InArguments)
		{
			FString Output;
			return RunGitExpectExit(InArguments, 0, Output);
		}

		bool RunGit(const FString& InArguments, FString& OutStandardOutput)
		{
			return RunGitExpectExit(InArguments, 0, OutStandardOutput);
		}

		bool RunGitExpectExit(const FString& InArguments, const int32 InExpectedExitCode, FString& OutStandardOutput)
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(RepositoryRoot), *InArguments);
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutStandardOutput, &StandardError);
			if (ReturnCode != InExpectedExitCode)
			{
				Test.AddError(FString::Printf(TEXT("Fixture Git command returned %d rather than %d: git %s\n%s"), ReturnCode, InExpectedExitCode, *CommandLine, *StandardError));
				return false;
			}
			return true;
		}

		FString AbsoluteFilename(const FString& InRelativeFilename) const
		{
			return FPaths::Combine(RepositoryRoot, InRelativeFilename);
		}

		FString RelativeFilename(const FString& InAbsoluteFilename) const
		{
			FString Result = FPaths::ConvertRelativePathToFull(InAbsoluteFilename);
			FPaths::NormalizeFilename(Result);
			FString RepositoryPrefix = RepositoryRoot;
			FPaths::NormalizeDirectoryName(RepositoryPrefix);
			RepositoryPrefix += TEXT("/");
			if (!FPaths::MakePathRelativeTo(Result, *RepositoryPrefix))
			{
				Test.AddError(FString::Printf(TEXT("Fixture path is outside the Git repository: %s"), *InAbsoluteFilename));
				return FString();
			}
			FPaths::NormalizeFilename(Result);
			return Result;
		}

		const FString& GetGitBinary() const
		{
			return GitBinary;
		}

		const FString& GetRepositoryRoot() const
		{
			return RepositoryRoot;
		}

	private:
		bool IsSafeFixturePath(const FString& InTempRoot) const
		{
			const FString ExpectedParent = FPaths::Combine(InTempRoot, TEXT("GitSourceControlAssetOperationTests"));
			if (!IsSameOrUnderDirectory(RepositoryRoot, ExpectedParent) || IsSameOrUnderDirectory(RepositoryRoot, FPaths::ProjectDir()))
			{
				return false;
			}
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (IsSameOrUnderDirectory(RepositoryRoot, Plugin->GetBaseDir()))
				{
					return false;
				}
			}
			return true;
		}

		void Cleanup()
		{
			if (RepositoryRoot.IsEmpty())
			{
				return;
			}
			if (!IsSafeFixturePath(NormalizeDirectory(FPlatformProcess::UserTempDir())))
			{
				Test.AddError(FString::Printf(TEXT("Refusing to delete unsafe Git asset-operation fixture: %s"), *RepositoryRoot));
				return;
			}
			if (IFileManager::Get().DirectoryExists(*RepositoryRoot) && !IFileManager::Get().DeleteDirectory(*RepositoryRoot, false, true))
			{
				Test.AddError(FString::Printf(TEXT("Failed to delete Git asset-operation fixture: %s"), *RepositoryRoot));
			}
			RepositoryRoot.Reset();
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString RepositoryRoot;
	};

	bool CreateCommittedTextFixture(FGitAssetOperationFixture& InFixture)
	{
		return InFixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("head revision\n"))
			&& InFixture.WriteFile(TEXT("Content/Unrelated.txt"), TEXT("unrelated\n"))
			&& InFixture.CommitAll(TEXT("Initial text fixture"));
	}

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakeAcceptingCallbacks(int32& OutConfirmCalls, int32& OutPrepareCalls, int32& OutReloadCalls)
	{
		using namespace GitSourceControlAssetOperations;
		FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [&OutConfirmCalls](const FString&, const TArray<FString>&)
		{
			++OutConfirmCalls;
			return true;
		};
		Callbacks.PrepareForMutation = [&OutPrepareCalls](const TArray<FString>&)
		{
			++OutPrepareCalls;
			return true;
		};
		Callbacks.ReloadPackages = [&OutReloadCalls](const TArray<FString>&)
		{
			++OutReloadCalls;
			return true;
		};
		return Callbacks;
	}

	bool TestCallbacksWereUsed(FAutomationTestBase& InTest, const int32 InConfirmCalls, const int32 InPrepareCalls, const int32 InReloadCalls)
	{
		return InTest.TestEqual(TEXT("Confirm callback runs once"), InConfirmCalls, 1)
			&& InTest.TestEqual(TEXT("Prepare callback runs once"), InPrepareCalls, 1)
			&& InTest.TestEqual(TEXT("Reload callback runs once"), InReloadCalls, 1);
	}

	bool IsGitDiffClean(FGitAssetOperationFixture& InFixture, const FString& InArguments)
	{
		FString Output;
		return InFixture.RunGitExpectExit(InArguments, 0, Output);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.Discard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture))
	{
		return false;
	}

	const FString RelativeFilename = TEXT("Content/Tracked.txt");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	if (!Fixture.WriteFile(RelativeFilename, TEXT("working only change\n")))
	{
		return false;
	}

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Working-tree discard succeeds"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	FString Contents;
	TestTrue(TEXT("Discarded file is present"), Fixture.ReadFile(RelativeFilename, Contents));
	TestEqual(TEXT("Working-tree discard restores HEAD"), Contents, FString(TEXT("head revision\n")));
	TestTrue(TEXT("Working-tree is clean after discard"), IsGitDiffClean(Fixture, TEXT("diff --quiet -- Content/Tracked.txt")));
	TestTrue(TEXT("Index is clean after working-tree discard"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.txt")));
	if (!TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls))
	{
		return false;
	}

	if (!Fixture.WriteFile(RelativeFilename, TEXT("staged change\n")) || !Fixture.RunGit(TEXT("add -- Content/Tracked.txt")) || !Fixture.WriteFile(RelativeFilename, TEXT("staged plus working change\n")))
	{
		return false;
	}
	ConfirmCalls = 0;
	PrepareCalls = 0;
	ReloadCalls = 0;
	Result = FGitAssetOperationResult();
	if (!TestTrue(TEXT("Staged discard succeeds"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	TestTrue(TEXT("Staged discard restores file"), Fixture.ReadFile(RelativeFilename, Contents));
	TestEqual(TEXT("Staged discard restores HEAD content"), Contents, FString(TEXT("head revision\n")));
	TestTrue(TEXT("Working tree is clean after staged discard"), IsGitDiffClean(Fixture, TEXT("diff --quiet -- Content/Tracked.txt")));
	TestTrue(TEXT("Index is clean after staged discard"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.txt")));
	TestTrue(TEXT("Discard result reports disk success"), Result.bSucceeded);
	return TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDeletedTrackedAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardDeletedTracked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDeletedTrackedAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture) || !Fixture.DeleteFile(TEXT("Content/Tracked.txt")))
	{
		return false;
	}

	const FString Filename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt"));
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Deleted tracked file is restored"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	FString Contents;
	TestTrue(TEXT("Deleted tracked file exists after discard"), Fixture.ReadFile(TEXT("Content/Tracked.txt"), Contents));
	TestEqual(TEXT("Deleted tracked file is restored from HEAD"), Contents, FString(TEXT("head revision\n")));
	TestTrue(TEXT("Restored deleted file has no index deletion"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.txt")));
	return TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetRenameDiscardAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetRenameDiscardAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture))
	{
		return false;
	}

	struct FRenameScenario
	{
		FString Name;
		bool bStaged = false;
		bool bSelectOldPath = false;
	};
	const TArray<FRenameScenario> Scenarios
	{
		{ TEXT("staged old endpoint"), true, true },
		{ TEXT("staged new endpoint"), true, false },
		{ TEXT("worktree old endpoint"), false, true },
		{ TEXT("worktree new endpoint"), false, false },
	};
	const FString OldRelativeFilename = TEXT("Content/Tracked.txt");
	const FString NewRelativeFilename = TEXT("Content/Renamed.txt");
	const FString OldFilename = Fixture.AbsoluteFilename(OldRelativeFilename);
	const FString NewFilename = Fixture.AbsoluteFilename(NewRelativeFilename);
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());

	for (const FRenameScenario& Scenario : Scenarios)
	{
		const bool bRenamed = Scenario.bStaged
			? Fixture.RunGit(TEXT("mv -- Content/Tracked.txt Content/Renamed.txt"))
			: Fixture.MoveFile(OldRelativeFilename, NewRelativeFilename);
		if (!bRenamed)
		{
			return false;
		}

		int32 ConfirmCalls = 0;
		int32 PrepareCalls = 0;
		int32 ReloadCalls = 0;
		FGitAssetOperationResult Result;
		const FString& SelectedFilename = Scenario.bSelectOldPath ? OldFilename : NewFilename;
		if (!TestTrue(*FString::Printf(TEXT("Discard succeeds from %s"), *Scenario.Name), Operations.DiscardTrackedFiles({ SelectedFilename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
		{
			AddError(FString::Join(Result.Errors, TEXT("\n")));
			return false;
		}

		FString RestoredContents;
		TestTrue(*FString::Printf(TEXT("Old path is restored from %s"), *Scenario.Name), Fixture.ReadFile(OldRelativeFilename, RestoredContents));
		TestEqual(*FString::Printf(TEXT("Old path restores HEAD content from %s"), *Scenario.Name), RestoredContents, FString(TEXT("head revision\n")));
		TestFalse(*FString::Printf(TEXT("New path is removed from %s"), *Scenario.Name), IFileManager::Get().FileExists(*NewFilename));
		TestTrue(*FString::Printf(TEXT("Index has no staged rename from %s"), *Scenario.Name), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.txt Content/Renamed.txt")));
		FString Status;
		if (!TestTrue(*FString::Printf(TEXT("Working tree is clean after %s"), *Scenario.Name), Fixture.RunGit(TEXT("status --porcelain=v2 --untracked-files=all"), Status)))
		{
			return false;
		}
		Status.TrimStartAndEndInline();
		TestTrue(*FString::Printf(TEXT("No rename residue remains after %s"), *Scenario.Name), Status.IsEmpty());
		if (!TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls))
		{
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardHeadChangeAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardHeadChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardHeadChangeAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture) || !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("local discard candidate\n")))
	{
		return false;
	}

	const FString RelativeFilename = TEXT("Content/Tracked.txt");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexBefore))
	{
		return false;
	}

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationCallbacks Callbacks;
	Callbacks.Confirm = [&Fixture, &ConfirmCalls](const FString&, const TArray<FString>&)
	{
		++ConfirmCalls;
		return Fixture.WriteFile(TEXT("Content/Unrelated.txt"), TEXT("external GUI changed HEAD\n"))
			&& Fixture.RunGit(TEXT("add -- Content/Unrelated.txt"))
			&& Fixture.RunGit(TEXT("commit --no-gpg-sign -m \"External HEAD update\""));
	};
	Callbacks.PrepareForMutation = [&PrepareCalls](const TArray<FString>&)
	{
		++PrepareCalls;
		return true;
	};
	Callbacks.ReloadPackages = [&ReloadCalls](const TArray<FString>&)
	{
		++ReloadCalls;
		return true;
	};

	FGitAssetOperationResult Result;
	TestFalse(TEXT("Discard aborts when HEAD changes during confirmation"), Operations.DiscardTrackedFiles({ Filename }, Callbacks, Result));
	FString ContentsAfter;
	FString IndexAfter;
	TestTrue(TEXT("HEAD-change rejection keeps target readable"), Fixture.ReadFile(RelativeFilename, ContentsAfter));
	TestEqual(TEXT("HEAD-change rejection leaves worktree untouched"), ContentsAfter, FString(TEXT("local discard candidate\n")));
	TestTrue(TEXT("HEAD-change rejection leaves target index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexAfter));
	TestEqual(TEXT("HEAD-change rejection leaves target index untouched"), IndexAfter, IndexBefore);
	TestEqual(TEXT("HEAD-change rejection prompts once"), ConfirmCalls, 1);
	TestEqual(TEXT("HEAD-change rejection prepares before the final recheck"), PrepareCalls, 1);
	TestEqual(TEXT("HEAD-change rejection does not reload packages"), ReloadCalls, 0);
	return TestTrue(TEXT("HEAD-change rejection reports the changed boundary"), Result.Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("HEAD"));
	}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardFinalIndexRecheckAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardFinalIndexRecheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardFinalIndexRecheckAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture) || !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("local discard candidate\n")) ||
		!Fixture.WriteFile(TEXT("Content/IndexOnly.txt"), TEXT("index only replacement\n")))
	{
		return false;
	}

	FString ReplacementBlob;
	if (!Fixture.RunGit(TEXT("hash-object -w Content/IndexOnly.txt"), ReplacementBlob))
	{
		return false;
	}
	ReplacementBlob.TrimStartAndEndInline();
	if (ReplacementBlob.IsEmpty() || !Fixture.DeleteFile(TEXT("Content/IndexOnly.txt")))
	{
		return false;
	}

	const FString RelativeFilename = TEXT("Content/Tracked.txt");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexBefore))
	{
		return false;
	}

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 PrepareCalls = 0;
	FGitAssetOperationCallbacks Callbacks;
	Callbacks.Confirm = [](const FString&, const TArray<FString>&)
	{
		return true;
	};
	Callbacks.PrepareForMutation = [&Fixture, &PrepareCalls, ReplacementBlob](const TArray<FString>&)
	{
		++PrepareCalls;
		return Fixture.RunGit(FString::Printf(TEXT("update-index --add --cacheinfo 100644,%s,Content/Tracked.txt"), *ReplacementBlob));
	};

	FGitAssetOperationResult Result;
	TestFalse(TEXT("Discard aborts when the index changes during Game Thread preparation"), Operations.DiscardTrackedFiles({ Filename }, Callbacks, Result));
	FString ContentsAfter;
	FString IndexAfter;
	TestTrue(TEXT("Final-index rejection keeps target readable"), Fixture.ReadFile(RelativeFilename, ContentsAfter));
	TestEqual(TEXT("Final-index rejection leaves worktree untouched"), ContentsAfter, FString(TEXT("local discard candidate\n")));
	TestTrue(TEXT("Final-index rejection keeps the external index change"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexAfter));
	TestFalse(TEXT("Final-index rejection does not roll back external index changes"), IndexAfter.Equals(IndexBefore, ESearchCase::CaseSensitive));
	TestEqual(TEXT("Final-index rejection prepares once"), PrepareCalls, 1);
	return TestTrue(TEXT("Final-index rejection reports the index boundary"), Result.Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("index"), ESearchCase::IgnoreCase);
	}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardRenameTextFilterAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardRenameTextFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardRenameTextFilterAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.RunGit(TEXT("config core.autocrlf false"))
		|| !Fixture.WriteFile(TEXT(".gitattributes"), TEXT("Content/*.txt text eol=lf\n"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("head text\n"))
		|| !Fixture.CommitAll(TEXT("Initial text-filter fixture")))
	{
		return false;
	}

	const FString OldRelativeFilename = TEXT("Content/Tracked.txt");
	const FString NewRelativeFilename = TEXT("Content/Renamed.txt");
	const FString OldFilename = Fixture.AbsoluteFilename(OldRelativeFilename);
	const FString NewFilename = Fixture.AbsoluteFilename(NewRelativeFilename);
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	for (const bool bSelectOldPath : { true, false })
	{
		if (!Fixture.WriteFile(OldRelativeFilename, TEXT("head text\r\n")) || !Fixture.MoveFile(OldRelativeFilename, NewRelativeFilename))
		{
			return false;
		}
		int32 ConfirmCalls = 0;
		int32 PrepareCalls = 0;
		int32 ReloadCalls = 0;
		FGitAssetOperationResult Result;
		const FString& SelectedFilename = bSelectOldPath ? OldFilename : NewFilename;
		if (!TestTrue(bSelectOldPath ? TEXT("CRLF rename discard succeeds from old path") : TEXT("CRLF rename discard succeeds from new path"),
			Operations.DiscardTrackedFiles({ SelectedFilename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
		{
			AddError(FString::Join(Result.Errors, TEXT("\n")));
			return false;
		}
		TestTrue(bSelectOldPath ? TEXT("CRLF rename restores old path from old selection") : TEXT("CRLF rename restores old path from new selection"), IFileManager::Get().FileExists(*OldFilename));
		TestFalse(bSelectOldPath ? TEXT("CRLF rename removes new path from old selection") : TEXT("CRLF rename removes new path from new selection"), IFileManager::Get().FileExists(*NewFilename));
		TestTrue(bSelectOldPath ? TEXT("CRLF old selection leaves clean index") : TEXT("CRLF new selection leaves clean index"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.txt Content/Renamed.txt")));
		FString Status;
		if (!TestTrue(bSelectOldPath ? TEXT("CRLF old selection status is readable") : TEXT("CRLF new selection status is readable"), Fixture.RunGit(TEXT("status --porcelain=v2 --untracked-files=all"), Status)))
		{
			return false;
		}
		Status.TrimStartAndEndInline();
		TestTrue(bSelectOldPath ? TEXT("CRLF old selection leaves no residue") : TEXT("CRLF new selection leaves no residue"), Status.IsEmpty());
		if (!TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls))
		{
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardRenameLfsAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DiscardRenameLfs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardRenameLfsAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.RunGit(TEXT("lfs install --local"))
		|| !Fixture.RunGit(TEXT("lfs track \"Content/*.uasset\""))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("materialized local LFS content\n"))
		|| !Fixture.CommitAll(TEXT("Initial local LFS fixture")))
	{
		return false;
	}

	const FString OldRelativeFilename = TEXT("Content/Tracked.uasset");
	const FString NewRelativeFilename = TEXT("Content/Renamed.uasset");
	const FString OldFilename = Fixture.AbsoluteFilename(OldRelativeFilename);
	const FString NewFilename = Fixture.AbsoluteFilename(NewRelativeFilename);
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	for (const bool bSelectOldPath : { true, false })
	{
		if (!Fixture.MoveFile(OldRelativeFilename, NewRelativeFilename))
		{
			return false;
		}
		int32 ConfirmCalls = 0;
		int32 PrepareCalls = 0;
		int32 ReloadCalls = 0;
		FGitAssetOperationResult Result;
		const FString& SelectedFilename = bSelectOldPath ? OldFilename : NewFilename;
		if (!TestTrue(bSelectOldPath ? TEXT("Local LFS rename discard succeeds from old path") : TEXT("Local LFS rename discard succeeds from new path"),
			Operations.DiscardTrackedFiles({ SelectedFilename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
		{
			AddError(FString::Join(Result.Errors, TEXT("\n")));
			return false;
		}
		FString RestoredContents;
		TestTrue(bSelectOldPath ? TEXT("Local LFS old selection materializes restored asset") : TEXT("Local LFS new selection materializes restored asset"), Fixture.ReadFile(OldRelativeFilename, RestoredContents));
		TestEqual(bSelectOldPath ? TEXT("Local LFS old selection restores content") : TEXT("Local LFS new selection restores content"), RestoredContents, FString(TEXT("materialized local LFS content\n")));
		TestFalse(bSelectOldPath ? TEXT("Local LFS old selection removes new path") : TEXT("Local LFS new selection removes new path"), IFileManager::Get().FileExists(*NewFilename));
		TestTrue(bSelectOldPath ? TEXT("Local LFS old selection leaves clean index") : TEXT("Local LFS new selection leaves clean index"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- Content/Tracked.uasset Content/Renamed.uasset")));
		FString Status;
		if (!TestTrue(bSelectOldPath ? TEXT("Local LFS old selection status is readable") : TEXT("Local LFS new selection status is readable"), Fixture.RunGit(TEXT("status --porcelain=v2 --untracked-files=all"), Status)))
		{
			return false;
		}
		Status.TrimStartAndEndInline();
		TestTrue(bSelectOldPath ? TEXT("Local LFS old selection leaves no residue") : TEXT("Local LFS new selection leaves no residue"), Status.IsEmpty());
		if (!TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls))
		{
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDeleteNewAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.DeleteNew",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDeleteNewAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture)
		|| !Fixture.WriteFile(TEXT("Content/Index Added.txt"), TEXT("index added\n"))
		|| !Fixture.WriteFile(TEXT("Content/Leave Me.txt"), TEXT("untracked but unselected\n"))
		|| !Fixture.RunGit(TEXT("add -- \"Content/Index Added.txt\"")))
	{
		return false;
	}

	const FString SelectedFilename = Fixture.AbsoluteFilename(TEXT("Content/Index Added.txt"));
	const FString UnselectedFilename = Fixture.AbsoluteFilename(TEXT("Content/Leave Me.txt"));
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Index-added selected file is deleted"), Operations.DeleteUntrackedFiles({ SelectedFilename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	TestFalse(TEXT("Selected index-added file is removed from disk"), IFileManager::Get().FileExists(*SelectedFilename));
	TestTrue(TEXT("Selected index-added file is removed from index"), IsGitDiffClean(Fixture, TEXT("diff --cached --quiet -- \"Content/Index Added.txt\"")));
	TestTrue(TEXT("Unselected untracked file remains on disk"), IFileManager::Get().FileExists(*UnselectedFilename));
	FString UntrackedProbeOutput;
	if (!Fixture.RunGitExpectExit(TEXT("ls-files --error-unmatch -- \"Content/Leave Me.txt\""), 1, UntrackedProbeOutput))
	{
		return false;
	}
	return TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetHistoryRestoreAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.HistoryRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetHistoryRestoreAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture))
	{
		return false;
	}

	FString CommitId;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), CommitId))
	{
		return false;
	}
	CommitId.TrimStartAndEndInline();

	const FString RelativeFilename = TEXT("Content/Tracked.txt");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	if (!Fixture.WriteFile(RelativeFilename, TEXT("staged revision\n")) || !Fixture.RunGit(TEXT("add -- Content/Tracked.txt")) || !Fixture.WriteFile(RelativeFilename, TEXT("unstaged revision\n")))
	{
		return false;
	}
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexBefore))
	{
		return false;
	}

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Historical text revision restores to worktree"), Operations.RestoreRevisionToWorkspace(Filename, CommitId, RelativeFilename, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}

	FString Contents;
	TestTrue(TEXT("Historical restore writes the target"), Fixture.ReadFile(RelativeFilename, Contents));
	TestEqual(TEXT("Historical restore uses requested commit content"), Contents, FString(TEXT("head revision\n")));
	FString IndexAfter;
	TestTrue(TEXT("Index can be queried after historical restore"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexAfter));
	TestEqual(TEXT("Historical restore leaves index byte-for-byte unchanged"), IndexAfter, IndexBefore);
	TestTrue(TEXT("Historical restore reports disk success"), Result.bSucceeded);
	return TestCallbacksWereUsed(*this, ConfirmCalls, PrepareCalls, ReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetRejectionAutomationTest,
	"Cthulhu.GitSourceControl.AssetOperations.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetRejectionAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;

	FGitAssetOperationFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedTextFixture(Fixture))
	{
		return false;
	}

	const FString UntrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/Untracked.txt"));
	if (!Fixture.WriteFile(TEXT("Content/Untracked.txt"), TEXT("untracked\n")))
	{
		return false;
	}
	FString CommitId;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), CommitId))
	{
		return false;
	}
	CommitId.TrimStartAndEndInline();

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRepositoryRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	const FGitAssetOperationCallbacks Callbacks = MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls);
	TestFalse(TEXT("Untracked file cannot use tracked discard"), Operations.DiscardTrackedFiles({ UntrackedFilename }, Callbacks, Result));
	TestFalse(TEXT("Untracked file cannot use historical restore"), Operations.RestoreRevisionToWorkspace(UntrackedFilename, CommitId, TEXT("Content/Tracked.txt"), Callbacks, Result));
	TestEqual(TEXT("Rejected untracked operations do not prompt"), ConfirmCalls, 0);
	TestEqual(TEXT("Rejected untracked operations do not prepare mutation"), PrepareCalls, 0);
	TestEqual(TEXT("Rejected untracked operations do not reload"), ReloadCalls, 0);
	TestTrue(TEXT("Untracked file remains after rejection"), IFileManager::Get().FileExists(*UntrackedFilename));

	FString Branch;
	if (!Fixture.RunGit(TEXT("branch --show-current"), Branch))
	{
		return false;
	}
	Branch.TrimStartAndEndInline();
	if (Branch.IsEmpty()
		|| !Fixture.RunGit(TEXT("checkout -b conflict-side"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("conflict side\n"))
		|| !Fixture.CommitAll(TEXT("Conflict side"))
		|| !Fixture.RunGit(FString::Printf(TEXT("checkout %s"), *QuoteGitArgument(Branch)))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("current side\n"))
		|| !Fixture.CommitAll(TEXT("Current side")))
	{
		return false;
	}

	FString MergeOutput;
	if (!Fixture.RunGitExpectExit(TEXT("merge conflict-side"), 1, MergeOutput))
	{
		return false;
	}
	const FString TrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt"));
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexBefore))
	{
		return false;
	}
	FString ContentsBefore;
	if (!Fixture.ReadFile(TEXT("Content/Tracked.txt"), ContentsBefore))
	{
		return false;
	}
	Result = FGitAssetOperationResult();
	TestFalse(TEXT("Conflicted file cannot be discarded"), Operations.DiscardTrackedFiles({ TrackedFilename }, Callbacks, Result));
	FString IndexAfter;
	FString ContentsAfter;
	TestTrue(TEXT("Conflict index remains readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.txt"), IndexAfter));
	TestTrue(TEXT("Conflict worktree remains readable"), Fixture.ReadFile(TEXT("Content/Tracked.txt"), ContentsAfter));
	TestEqual(TEXT("Conflict rejection leaves index unchanged"), IndexAfter, IndexBefore);
	TestEqual(TEXT("Conflict rejection leaves worktree unchanged"), ContentsAfter, ContentsBefore);
	TestEqual(TEXT("Conflict rejection does not prompt"), ConfirmCalls, 0);
	TestEqual(TEXT("Conflict rejection does not prepare mutation"), PrepareCalls, 0);
	TestEqual(TEXT("Conflict rejection does not reload"), ReloadCalls, 0);
	return true;
}

#endif
