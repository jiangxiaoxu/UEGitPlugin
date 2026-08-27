// Copyright (c) 2026

#include "GitChangedAssetOperations.h"
#include "GitChangedAssetsStatus.h"
#include "GitRepositoryMutationGuard.h"
#include "GitSourceControlUtils.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitChangedAssetOperationsAutomationTestsPrivate
{
	FString QuoteGitArgument(const FString& InArgument)
	{
		FString Escaped = InArgument;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
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
			Root = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetOperationsTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IsSafePath() || !IFileManager::Get().MakeDirectory(*Root, true))
			{
				Test.AddError(FString::Printf(TEXT("Could not create safe Changed Assets fixture: %s"), *Root));
				return false;
			}
			return RunGit(TEXT("init")) && RunGit(TEXT("config user.name \"GitChangedAssetsTests\"")) && RunGit(TEXT("config user.email \"git-changed-assets@example.invalid\""));
		}

		bool WriteFile(const FString& InRelativeFilename, const FString& InContents) const
		{
			const FString Filename = AbsoluteFilename(InRelativeFilename);
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)
				&& FFileHelper::SaveStringToFile(InContents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool ReadFile(const FString& InRelativeFilename, FString& OutContents) const
		{
			return FFileHelper::LoadFileToString(OutContents, *AbsoluteFilename(InRelativeFilename));
		}

		bool RunGit(const FString& InArguments, FString& OutOutput) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString Arguments = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *InArguments);
			FPlatformProcess::ExecProcess(*GitBinary, *Arguments, &ReturnCode, &OutOutput, &StandardError);
			if (ReturnCode == 0)
			{
				return true;
			}
			Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *Arguments, *StandardError));
			return false;
		}

		bool RunGit(const FString& InArguments) const
		{
			FString Output;
			return RunGit(InArguments, Output);
		}

		bool CommitAll(const FString& InMessage) const
		{
			return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(InMessage)));
		}

		FString AbsoluteFilename(const FString& InRelativeFilename) const { return FPaths::Combine(Root, InRelativeFilename); }
		const FString& GetRoot() const { return Root; }
		const FString& GetGitBinary() const { return GitBinary; }

	private:
		bool IsSafePath() const
		{
			const FString Parent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitChangedAssetOperationsTests"));
			if (!IsSameOrUnderDirectory(Root, Parent) || IsSameOrUnderDirectory(Root, FPaths::ProjectDir()))
			{
				return false;
			}
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (IsSameOrUnderDirectory(Root, Plugin->GetBaseDir()))
				{
					return false;
				}
			}
			return true;
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString Root;
	};

	FGitChangedAssetEntry MakeEntry(const FFixture& InFixture, const FString& InRelativeFilename, const EGitChangedAssetState InState)
	{
		FGitChangedAssetEntry Entry;
		Entry.RepositoryRelativePath = InRelativeFilename;
		Entry.AbsoluteFilename = InFixture.AbsoluteFilename(InRelativeFilename);
		Entry.State = InState;
		Entry.PackageKind = EGitChangedAssetPackageKind::Regular;
		Entry.bBaseRevertEligible = true;
		Entry.bCanRevert = true;
		Entry.bMetadataResolved = true;
		switch (InState)
		{
		case EGitChangedAssetState::Modified:
			Entry.IndexStatus = TEXT('M');
			Entry.WorktreeStatus = TEXT('M');
			break;
		case EGitChangedAssetState::Deleted:
			Entry.IndexStatus = TEXT('D');
			break;
		case EGitChangedAssetState::Added:
			Entry.IndexStatus = TEXT('A');
			break;
		case EGitChangedAssetState::Untracked:
			Entry.IndexStatus = TEXT('?');
			Entry.WorktreeStatus = TEXT('?');
			break;
		case EGitChangedAssetState::Renamed:
			Entry.IndexStatus = TEXT('R');
			break;
		default:
			break;
		}
		return Entry;
	}

	GitChangedAssetOperations::FGitChangedAssetRevertCallbacks MakeCallbacks(int32& OutConfirmCalls, int32& OutPrepareCalls, int32& OutBeginMutationCalls, int32& OutFinalizeCalls,
		TArray<GitChangedAssetOperations::EGitChangedAssetMutationOutcome>* InOutOutcomes = nullptr)
	{
		GitChangedAssetOperations::FGitChangedAssetRevertCallbacks Callbacks;
		Callbacks.Confirm = [&OutConfirmCalls](const TArray<FGitChangedAssetEntry>&, FString&)
		{
			++OutConfirmCalls;
			return true;
		};
		Callbacks.PrepareForMutation = [&OutPrepareCalls](const TArray<FGitChangedAssetEntry>&, FString&)
		{
			++OutPrepareCalls;
			return true;
		};
		Callbacks.BeginMutation = [&OutBeginMutationCalls](const TArray<FGitChangedAssetEntry>&, FString&)
		{
			++OutBeginMutationCalls;
			return true;
		};
		Callbacks.FinalizeEditor = [&OutFinalizeCalls, InOutOutcomes](const TArray<FGitChangedAssetEntry>&, const TArray<FString>&,
			const GitChangedAssetOperations::EGitChangedAssetMutationOutcome Outcome, FString&)
		{
			++OutFinalizeCalls;
			if (InOutOutcomes != nullptr)
			{
				InOutOutcomes->Add(Outcome);
			}
			return true;
		};
		return Callbacks;
	}

	bool CaptureRevertEntries(FAutomationTestBase& InTest, const FFixture& InFixture, FString& InOutPinnedHead, TArray<FGitChangedAssetEntry>& OutEntries)
	{
		FGitChangedAssetSnapshot Snapshot;
		FString Error;
		if (!FGitChangedAssetsStatus::CaptureSnapshot(InFixture.GetGitBinary(), InFixture.GetRoot(), 1, Snapshot, Error))
		{
			InTest.AddError(Error);
			return false;
		}
		if (!InOutPinnedHead.Equals(Snapshot.PinnedHead, ESearchCase::CaseSensitive))
		{
			InTest.AddError(TEXT("The mutation fixture HEAD changed before the Changed Assets snapshot was captured."));
			return false;
		}
		OutEntries = MoveTemp(Snapshot.Entries);
		for (FGitChangedAssetEntry& Entry : OutEntries)
		{
			Entry.bMetadataResolved = true;
			Entry.bCanRevert = Entry.bBaseRevertEligible;
			Entry.PackageKind = EGitChangedAssetPackageKind::Regular;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetRevertToHeadAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.RevertToHead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetRevertToHeadAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Modified.uasset"), TEXT("head modified\n"))
		|| !Fixture.WriteFile(TEXT("Content/Deleted.uasset"), TEXT("head deleted\n"))
		|| !Fixture.WriteFile(TEXT("Content/OldName.uasset"), TEXT("head renamed\n"))
		|| !Fixture.WriteFile(TEXT("Content/[A]/Asset.uasset"), TEXT("head literal selected\n"))
		|| !Fixture.WriteFile(TEXT("Content/A/Asset.uasset"), TEXT("head literal neighbor\n"))
		|| !Fixture.CommitAll(TEXT("Changed Assets initial uassets")))
	{
		return false;
	}
	FString PinnedHead;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)) return false;
	PinnedHead.TrimStartAndEndInline();
	if (!Fixture.WriteFile(TEXT("Content/Modified.uasset"), TEXT("staged modified\n"))
		|| !Fixture.RunGit(TEXT("add -- Content/Modified.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Modified.uasset"), TEXT("unstaged modified\n"))
		|| !Fixture.RunGit(TEXT("rm -- Content/Deleted.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Deleted.uasset"), TEXT("untracked replacement after staged delete\n"))
		|| !Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("added content\n"))
		|| !Fixture.RunGit(TEXT("add -- Content/Added.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("unstaged added content\n"))
		|| !Fixture.WriteFile(TEXT("Content/Untracked.uasset"), TEXT("untracked content\n"))
		|| !Fixture.RunGit(TEXT("mv -- Content/OldName.uasset Content/Renamed.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/[A]/Asset.uasset"), TEXT("staged literal selected\n"))
		|| !Fixture.RunGit(TEXT("--literal-pathspecs add -- Content/[A]/Asset.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/[A]/Asset.uasset"), TEXT("unstaged literal selected\n"))
		|| !Fixture.WriteFile(TEXT("Content/A/Asset.uasset"), TEXT("dirty literal neighbor\n")))
	{
		return false;
	}

	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	Entries.RemoveAll([](const FGitChangedAssetEntry& Entry)
	{
		return Entry.RepositoryRelativePath.Equals(TEXT("Content/A/Asset.uasset"), ESearchCase::CaseSensitive);
	});
	TestTrue(TEXT("Staged deletion retains its same-path untracked replacement topology"), Entries.ContainsByPredicate([](const FGitChangedAssetEntry& Entry)
	{
		return Entry.RepositoryRelativePath.Equals(TEXT("Content/Deleted.uasset"), ESearchCase::CaseSensitive) && Entry.bHasUntrackedReplacement;
	}));

	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 BeginMutationCalls = 0;
	int32 FinalizeCalls = 0;
	FGitChangedAssetRevertResult Result;
	FGitChangedAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	if (!TestTrue(TEXT("Mixed Changed Assets Revert succeeds"), Operations.RevertToHead(PinnedHead, Entries, MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	TestTrue(TEXT("Mixed Changed Assets transaction reports disk success"), Result.bSucceeded);
	TestTrue(TEXT("Mixed Changed Assets transaction reload succeeds"), Result.bReloadSucceeded);
	TestEqual(TEXT("Confirm runs once after baseline capture"), ConfirmCalls, 1);
	TestEqual(TEXT("Prepare runs once"), PrepareCalls, 1);
	TestEqual(TEXT("BeginMutation runs once at commit point"), BeginMutationCalls, 1);
	TestEqual(TEXT("Finalize runs once"), FinalizeCalls, 1);
	FString Contents;
	TestTrue(TEXT("Modified asset exists after Revert"), Fixture.ReadFile(TEXT("Content/Modified.uasset"), Contents));
	TestEqual(TEXT("Modified asset returns to HEAD"), Contents, FString(TEXT("head modified\n")));
	TestTrue(TEXT("Deleted asset returns from HEAD"), Fixture.ReadFile(TEXT("Content/Deleted.uasset"), Contents));
	TestEqual(TEXT("Deleted asset returns to HEAD"), Contents, FString(TEXT("head deleted\n")));
	TestTrue(TEXT("Rename source returns from HEAD"), Fixture.ReadFile(TEXT("Content/OldName.uasset"), Contents));
	TestEqual(TEXT("Rename source returns to HEAD"), Contents, FString(TEXT("head renamed\n")));
	TestTrue(TEXT("Literal path selected asset returns from HEAD"), Fixture.ReadFile(TEXT("Content/[A]/Asset.uasset"), Contents));
	TestEqual(TEXT("Literal path selected asset returns to HEAD"), Contents, FString(TEXT("head literal selected\n")));
	TestTrue(TEXT("Literal path neighbor remains readable"), Fixture.ReadFile(TEXT("Content/A/Asset.uasset"), Contents));
	TestEqual(TEXT("Literal path neighbor is not restored by selected bracket path"), Contents, FString(TEXT("dirty literal neighbor\n")));
	TestFalse(TEXT("Added asset is deleted exactly"), IFileManager::Get().FileExists(*Fixture.AbsoluteFilename(TEXT("Content/Added.uasset"))));
	TestFalse(TEXT("Untracked asset is deleted exactly"), IFileManager::Get().FileExists(*Fixture.AbsoluteFilename(TEXT("Content/Untracked.uasset"))));
	TestFalse(TEXT("Rename destination is deleted exactly"), IFileManager::Get().FileExists(*Fixture.AbsoluteFilename(TEXT("Content/Renamed.uasset"))));
	FString Status;
	if (!Fixture.RunGit(TEXT("status --porcelain=v2"), Status)) return false;
	Status.TrimStartAndEndInline();
	return TestTrue(TEXT("Only the unselected literal-path neighbor remains changed"), Status.Contains(TEXT("Content/A/Asset.uasset")) && !Status.Contains(TEXT("Content/[A]/Asset.uasset")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetRevertRollbackAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.RevertRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetRevertRollbackAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("head tracked\n")) || !Fixture.CommitAll(TEXT("Changed Assets rollback initial asset"))) return false;
	FString PinnedHead;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)) return false;
	PinnedHead.TrimStartAndEndInline();
	if (!Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("staged added\n")) || !Fixture.RunGit(TEXT("add -- Content/Added.uasset"))) return false;
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Added.uasset"), IndexBefore)) return false;
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 BeginMutationCalls = 0;
	int32 FinalizeCalls = 0;
	TArray<EGitChangedAssetMutationOutcome> Outcomes;
	FGitChangedAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	FGitChangedAssetRevertCallbacks PreCommitCallbacks = MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls, &Outcomes);
	PreCommitCallbacks.BeforeCommitPointForTesting = [&Fixture]()
	{
		Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("outside commit-point worktree change\n"));
	};
	FGitChangedAssetRevertResult Result;
	TestFalse(TEXT("Changed Assets rejects a worktree change after confirmation but before the mutation commit point"), Operations.RevertToHead(PinnedHead, Entries, PreCommitCallbacks, Result));
	TestFalse(TEXT("Pre-commit failure does not report disk success"), Result.bSucceeded);
	TestEqual(TEXT("Pre-commit Confirm runs once"), ConfirmCalls, 1);
	TestEqual(TEXT("Pre-commit Prepare runs once"), PrepareCalls, 1);
	TestEqual(TEXT("Pre-commit failure never enters the loader-reset mutation phase"), BeginMutationCalls, 0);
	TestEqual(TEXT("Pre-commit Finalize runs once"), FinalizeCalls, 1);
	if (!TestEqual(TEXT("Pre-commit Finalize receives NeverMutated"), Outcomes.Num(), 1) || !TestEqual(TEXT("Pre-commit outcome preserves Editor state"), Outcomes[0], EGitChangedAssetMutationOutcome::NeverMutated)) return false;
	FString Contents;
	TestTrue(TEXT("Pre-commit rejection preserves the external worktree bytes"), Fixture.ReadFile(TEXT("Content/Added.uasset"), Contents));
	TestEqual(TEXT("Pre-commit rejection does not discard editor-equivalent worktree state"), Contents, FString(TEXT("outside commit-point worktree change\n")));
	FString IndexAfterPreCommit;
	if (!TestTrue(TEXT("Pre-commit rejection keeps the added index entry"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Added.uasset"), IndexAfterPreCommit)) || !TestEqual(TEXT("Pre-commit rejection preserves the exact index entry"), IndexAfterPreCommit, IndexBefore)) return false;

	ConfirmCalls = 0;
	PrepareCalls = 0;
	BeginMutationCalls = 0;
	FinalizeCalls = 0;
	Outcomes.Reset();
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	FGitChangedAssetRevertCallbacks Callbacks = MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls, &Outcomes);
	Callbacks.AllowFilesystemMutationForTesting = []() { return false; };
	Result = FGitChangedAssetRevertResult();
	TestFalse(TEXT("Changed Assets rollback seam rejects the filesystem delete"), Operations.RevertToHead(PinnedHead, Entries, Callbacks, Result));
	TestFalse(TEXT("Rollback failure does not report disk success"), Result.bSucceeded);
	TestEqual(TEXT("Rollback Confirm runs once"), ConfirmCalls, 1);
	TestEqual(TEXT("Rollback Prepare runs once"), PrepareCalls, 1);
	TestEqual(TEXT("Rollback BeginMutation runs once"), BeginMutationCalls, 1);
	TestEqual(TEXT("Rollback Finalize runs once"), FinalizeCalls, 1);
	if (!TestEqual(TEXT("Rollback Finalize reports RolledBack"), Outcomes.Num(), 1) || !TestEqual(TEXT("Rollback outcome is distinct from zero-mutation failure"), Outcomes[0], EGitChangedAssetMutationOutcome::RolledBack)) return false;
	FString IndexAfter;
	TestTrue(TEXT("Rollback leaves index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Added.uasset"), IndexAfter));
	TestEqual(TEXT("Rollback restores exact added index entry"), IndexAfter, IndexBefore);
	TestTrue(TEXT("Rollback restores the added worktree file"), Fixture.ReadFile(TEXT("Content/Added.uasset"), Contents));
	TestEqual(TEXT("Rollback preserves the added worktree bytes"), Contents, FString(TEXT("outside commit-point worktree change\n")));
	FGitChangedAssetEntry Conflict = MakeEntry(Fixture, TEXT("Content/Added.uasset"), EGitChangedAssetState::Conflicted);
	Conflict.bBaseRevertEligible = false;
	Conflict.bCanRevert = false;
	Conflict.RevertBlockReason = TEXT("Resolve the Git conflict before reverting this asset.");
	Result = FGitChangedAssetRevertResult();
	TestFalse(TEXT("Conflicted Changed Asset is blocked before mutation"), Operations.RevertToHead(PinnedHead, { Conflict }, MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls), Result));
	return TestEqual(TEXT("Conflict does not invoke additional lifecycle callbacks"), PrepareCalls, 1)
		&& TestEqual(TEXT("Conflict does not enter mutation"), BeginMutationCalls, 1)
		&& TestEqual(TEXT("Conflict does not confirm again"), ConfirmCalls, 1)
		&& TestEqual(TEXT("Conflict does not finalize"), FinalizeCalls, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetMutationGuardCancellationAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.MutationGuardCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetMutationGuardCancellationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	const FString RepositoryKey = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetOperationsTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	GitSourceControlRepositoryMutation::FGitRepositoryMutationGuard HoldingGuard(RepositoryKey);
	if (!TestTrue(TEXT("First guard acquires the repository mutex"), HoldingGuard.Acquire())) return false;
	TAtomic<bool> bCancel = false;
	TFuture<bool> WaitingAcquire = Async(EAsyncExecution::ThreadPool, [&RepositoryKey, &bCancel]()
	{
		GitSourceControlRepositoryMutation::FGitRepositoryMutationGuard WaitingGuard(RepositoryKey);
		return WaitingGuard.Acquire([&bCancel]() { return bCancel.Load(); });
	});
	FPlatformProcess::SleepNoStats(0.025f);
	bCancel.Store(true);
	if (!TestTrue(TEXT("Cancelled waiter leaves the shared mutation guard promptly"), WaitingAcquire.WaitFor(FTimespan::FromSeconds(2.0)))) return false;
	return TestFalse(TEXT("Cancelled waiter never acquires the repository mutation guard"), WaitingAcquire.Get());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetMixedLfsPreflightAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.MixedLfsPreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetMixedLfsPreflightAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	const FString LfsPointer = TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nsize 17\n");
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/00Plain.uasset"), TEXT("head plain bytes\n"))
		|| !Fixture.WriteFile(TEXT("Content/99LfsPointer.uasset"), LfsPointer)
		|| !Fixture.CommitAll(TEXT("Mixed local LFS preflight fixture")))
	{
		return false;
	}
	FString PinnedHead;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)) return false;
	PinnedHead.TrimStartAndEndInline();
	if (!Fixture.WriteFile(TEXT("Content/00Plain.uasset"), TEXT("dirty plain bytes\n"))
		|| !Fixture.WriteFile(TEXT("Content/99LfsPointer.uasset"), TEXT("dirty fake LFS bytes\n")))
	{
		return false;
	}
	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	Entries.Sort([](const FGitChangedAssetEntry& A, const FGitChangedAssetEntry& B)
	{
		return A.RepositoryRelativePath < B.RepositoryRelativePath;
	});
	if (!TestEqual(TEXT("Fixture presents a non-LFS path before the LFS pointer path"), Entries.Num(), 2)) return false;
	TestEqual(TEXT("First selected path is the non-LFS asset"), Entries[0].RepositoryRelativePath, FString(TEXT("Content/00Plain.uasset")));
	TestEqual(TEXT("Second selected path is the LFS pointer asset"), Entries[1].RepositoryRelativePath, FString(TEXT("Content/99LfsPointer.uasset")));

	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 BeginMutationCalls = 0;
	int32 FinalizeCalls = 0;
	FGitChangedAssetRevertResult Result;
	FGitChangedAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	TestFalse(TEXT("Missing later LFS object rejects the mixed selection before mutation"),
		Operations.RevertToHead(PinnedHead, Entries, MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls), Result));
	TestFalse(TEXT("Mixed LFS preflight failure does not report disk success"), Result.bSucceeded);
	TestEqual(TEXT("Mixed LFS preflight confirms after baseline capture"), ConfirmCalls, 1);
	TestEqual(TEXT("Mixed LFS preflight does not prepare Editor packages"), PrepareCalls, 0);
	TestEqual(TEXT("Mixed LFS preflight does not enter the mutation commit point"), BeginMutationCalls, 0);
	TestEqual(TEXT("Mixed LFS preflight does not finalize a lifecycle"), FinalizeCalls, 0);
	FString Contents;
	TestTrue(TEXT("Non-LFS first path remains unmodified on later LFS failure"), Fixture.ReadFile(TEXT("Content/00Plain.uasset"), Contents));
	TestEqual(TEXT("Non-LFS first path retains its dirty bytes"), Contents, FString(TEXT("dirty plain bytes\n")));
	TestTrue(TEXT("LFS pointer path remains unmodified on preflight failure"), Fixture.ReadFile(TEXT("Content/99LfsPointer.uasset"), Contents));
	TestEqual(TEXT("LFS pointer path retains its dirty bytes"), Contents, FString(TEXT("dirty fake LFS bytes\n")));
	return TestTrue(TEXT("Mixed LFS preflight reports a recoverable LFS failure"), !Result.Errors.IsEmpty() && Result.Errors[0].Contains(TEXT("LFS"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetEmptyLifecycleClosureAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.EmptyLifecycleClosure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetEmptyLifecycleClosureAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Unloaded.uasset"), TEXT("unloaded lifecycle bytes\n")) || !Fixture.CommitAll(TEXT("Empty lifecycle closure fixture"))) return false;
	FGitChangedAssetEntry Entry = MakeEntry(Fixture, TEXT("Content/Unloaded.uasset"), EGitChangedAssetState::Modified);
	FGitChangedAssetRevertLifecycle Lifecycle;
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	if (!TestTrue(TEXT("Unloaded asset builds a valid empty lifecycle preview"), FGitChangedAssetRevertLifecycle::BuildPreview({ Entry }, Preview, Error))) return false;
	TestTrue(TEXT("Unloaded lifecycle preview has an empty closure signature"), Preview.ClosureSignature.IsEmpty());
	if (!TestTrue(TEXT("Empty lifecycle closure can be recorded after confirmation"), Lifecycle.RecordConfirmedClosure({ Entry }, Error))) return false;
	if (!TestTrue(TEXT("Empty lifecycle closure prepares without loaded packages"), Lifecycle.Prepare({ Entry }, Error))) return false;
	if (!TestTrue(TEXT("Empty lifecycle closure enters the mutation commit point"), Lifecycle.BeginMutation({ Entry }, Error))) return false;
	return TestTrue(TEXT("Empty lifecycle closure finalizes without reload or discard"),
		Lifecycle.Finish({ Entry }, { Entry.AbsoluteFilename }, EGitChangedAssetMutationOutcome::NeverMutated, Error));
}

#endif
