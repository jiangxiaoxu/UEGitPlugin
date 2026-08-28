// Copyright (c) 2026

#include "GitChangedAssetOperations.h"
#include "GitChangedAssetsStatus.h"
#include "GitRepositoryMutationGuard.h"
#include "GitSourceControlUtils.h"

#include "Async/Async.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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

	class FScopedCurrentEditorWorldOverride final
	{
	public:
		explicit FScopedCurrentEditorWorldOverride(UWorld* InWorld)
		{
			GitChangedAssetOperations::FGitChangedAssetRevertLifecycle::SetCurrentEditorWorldForTesting(InWorld);
		}

		~FScopedCurrentEditorWorldOverride()
		{
			GitChangedAssetOperations::FGitChangedAssetRevertLifecycle::SetCurrentEditorWorldForTesting(nullptr);
		}
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

	bool IsGitLfsAvailable(const FString& InGitBinary)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*InGitBinary, TEXT("lfs version"), &ReturnCode, &StandardOutput, &StandardError);
		return ReturnCode == 0;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetRevertLfsBatchAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.RevertLfsBatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetRevertLfsBatchAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	if (!IsGitLfsAvailable(Fixture.GetGitBinary()))
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping RevertLfsBatch."));
		return true;
	}
	const FString FirstRelativeFilename = TEXT("Content/First.uasset");
	const FString SecondRelativeFilename = TEXT("Content/Second.uasset");
	const FString HeadContents = TEXT("Changed Assets shared LFS head bytes\n");
	if (!Fixture.RunGit(TEXT("lfs install --local"))
		|| !Fixture.RunGit(TEXT("lfs track \"Content/*.uasset\""))
		|| !Fixture.WriteFile(TEXT(".gitignore"), TEXT("Origin.git/\n"))
		|| !Fixture.WriteFile(FirstRelativeFilename, HeadContents)
		|| !Fixture.WriteFile(SecondRelativeFilename, HeadContents)
		|| !Fixture.CommitAll(TEXT("Changed Assets batch LFS fixture")))
	{
		return false;
	}
	const FString OriginDirectory = Fixture.AbsoluteFilename(TEXT("Origin.git"));
	if (!Fixture.RunGit(FString::Printf(TEXT("init --bare %s"), *QuoteGitArgument(OriginDirectory)))
		|| !Fixture.RunGit(FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(OriginDirectory)))
		|| !Fixture.RunGit(TEXT("push -u origin HEAD")))
	{
		return false;
	}
	FString PinnedHead;
	FString FirstPointerText;
	FString SecondPointerText;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)
		|| !Fixture.RunGit(TEXT("show HEAD:Content/First.uasset"), FirstPointerText)
		|| !Fixture.RunGit(TEXT("show HEAD:Content/Second.uasset"), SecondPointerText)) return false;
	PinnedHead.TrimStartAndEndInline();
	FString FirstOid;
	FString SecondOid;
	int64 FirstSize = 0;
	int64 SecondSize = 0;
	if (!TestTrue(TEXT("Changed Assets first LFS pointer parses"), ParseLfsPointer(FirstPointerText, FirstOid, FirstSize))
		|| !TestTrue(TEXT("Changed Assets second LFS pointer parses"), ParseLfsPointer(SecondPointerText, SecondOid, SecondSize))
		|| !TestTrue(TEXT("Changed Assets batch paths share one oid:size"), FirstOid.Equals(SecondOid, ESearchCase::CaseSensitive) && FirstSize == SecondSize)) return false;
	const FString ObjectFilename = FPaths::Combine(Fixture.GetRoot(), TEXT(".git/lfs/objects"), FirstOid.Left(2), FirstOid.Mid(2, 2), FirstOid);
	FString VerifyError;
	if (!TestTrue(TEXT("Changed Assets batch local object verifies"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetRoot(), ObjectFilename, FirstOid, FirstSize, VerifyError)))
	{
		AddError(VerifyError);
		return false;
	}
	auto WriteDirtyPair = [&Fixture, &FirstRelativeFilename, &SecondRelativeFilename](const FString& InFirstContents, const FString& InSecondContents)
	{
		return Fixture.WriteFile(FirstRelativeFilename, InFirstContents) && Fixture.WriteFile(SecondRelativeFilename, InSecondContents);
	};
	if (!WriteDirtyPair(TEXT("Changed Assets local hit first dirty\n"), TEXT("Changed Assets local hit second dirty\n"))) return false;
	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 BeginMutationCalls = 0;
	int32 FinalizeCalls = 0;
	FGitChangedAssetRevertResult Result;
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	if (!TestTrue(TEXT("Changed Assets same-oid batch local hit succeeds"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(
		PinnedHead, Entries, MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	if (!TestEqual(TEXT("Changed Assets same-oid local hit verifies once"), GitSourceControlUtils::Testing::GetGitLfsVerifyLaunchCount(), static_cast<uint64>(1))
		|| !TestEqual(TEXT("Changed Assets same-oid local hit fetches zero times"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(0))
		|| !TestEqual(TEXT("Changed Assets same-oid local hit confirms once"), ConfirmCalls, 1)
		|| !TestEqual(TEXT("Changed Assets same-oid local hit prepares once"), PrepareCalls, 1)
		|| !TestEqual(TEXT("Changed Assets same-oid local hit enters mutation once"), BeginMutationCalls, 1)
		|| !TestEqual(TEXT("Changed Assets same-oid local hit finalizes once"), FinalizeCalls, 1)) return false;
	if (!TestTrue(TEXT("Remove Changed Assets shared LFS object before batch miss"), IFileManager::Get().Delete(*ObjectFilename, false, true, true))
		|| !WriteDirtyPair(TEXT("Changed Assets local miss first dirty\n"), TEXT("Changed Assets local miss second dirty\n"))) return false;
	Entries.Reset();
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	ConfirmCalls = 0;
	PrepareCalls = 0;
	BeginMutationCalls = 0;
	FinalizeCalls = 0;
	Result = FGitChangedAssetRevertResult();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	if (!TestTrue(TEXT("Changed Assets same-oid batch local miss succeeds"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(
		PinnedHead, Entries, MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	return TestEqual(TEXT("Changed Assets same-oid local miss fetches once"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(1))
		&& TestEqual(TEXT("Changed Assets same-oid local miss verifies once"), GitSourceControlUtils::Testing::GetGitLfsVerifyLaunchCount(), static_cast<uint64>(1))
		&& TestEqual(TEXT("Changed Assets same-oid local miss confirms once"), ConfirmCalls, 1)
		&& TestEqual(TEXT("Changed Assets same-oid local miss prepares once"), PrepareCalls, 1)
		&& TestEqual(TEXT("Changed Assets same-oid local miss enters mutation once"), BeginMutationCalls, 1)
		&& TestEqual(TEXT("Changed Assets same-oid local miss finalizes once"), FinalizeCalls, 1);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetInactiveOfpaLifecycleAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.InactiveOfpaLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetInactiveOfpaLifecycleAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	for (int32 EntryIndex = 0; EntryIndex < 100; ++EntryIndex)
	{
		if (!Fixture.WriteFile(FString::Printf(TEXT("Content/__ExternalActors__/Map/A/B/INACTIVE_%d.uasset"), EntryIndex),
			FString::Printf(TEXT("head inactive OFPA %d\n"), EntryIndex))) return false;
	}
	if (!Fixture.CommitAll(TEXT("Inactive OFPA batch lifecycle fixture"))) return false;
	FString PinnedHead;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)) return false;
	PinnedHead.TrimStartAndEndInline();

	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString DestinationOwnerLevel = FString::Printf(TEXT("/Temp/GitChangedAssetsInactiveDestination_%s"), *TestId);
	const FString SourceOwnerLevel = FString::Printf(TEXT("/Game/GitChangedAssetsInactiveSource_%s"), *TestId);
	UPackage* const InactiveDestinationOwnerPackage = CreatePackage(*DestinationOwnerLevel);
	UPackage* const InactiveSourceOwnerPackage = CreatePackage(*SourceOwnerLevel);
	if (!TestNotNull(TEXT("Creates an isolated inactive destination owner package"), InactiveDestinationOwnerPackage) ||
		!TestNotNull(TEXT("Creates an isolated inactive source owner package"), InactiveSourceOwnerPackage)) return false;
	InactiveDestinationOwnerPackage->SetDirtyFlag(false);
	InactiveSourceOwnerPackage->SetDirtyFlag(false);

	FGitChangedAssetEntry Entry = MakeEntry(Fixture, TEXT("Content/__ExternalActors__/Map/A/B/INACTIVE_0.uasset"), EGitChangedAssetState::Modified);
	Entry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
	Entry.OwnerLevel = DestinationOwnerLevel;
	Entry.bOwnerLevelResolved = true;
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	if (!TestTrue(TEXT("A clean inactive OFPA owner takes the disk-only lifecycle path"), FGitChangedAssetRevertLifecycle::BuildPreview({ Entry }, Preview, Error))) return false;
	TestTrue(TEXT("The disk-only OFPA path does not reload an inactive owner map"), Preview.OwnerMapsToReload.IsEmpty());
	int32 ReloadBatchCalls = 0;
	FGitChangedAssetRevertLifecycle::SetReloadPackagesForTesting([&ReloadBatchCalls](const TArray<UPackage*>&, FString&)
	{
		++ReloadBatchCalls;
		return true;
	});
	ON_SCOPE_EXIT { FGitChangedAssetRevertLifecycle::SetReloadPackagesForTesting({}); };
	struct FWarmTiming
	{
		double TotalMilliseconds = 0.0;
		double ConfirmMilliseconds = 0.0;
		double PrepareMilliseconds = 0.0;
		double BeginMutationMilliseconds = 0.0;
		double FinalizeMilliseconds = 0.0;
	};
	auto RevertInactiveBatch = [this, &Fixture, &PinnedHead, &DestinationOwnerLevel, &ReloadBatchCalls](const int32 InEntryCount, FWarmTiming& OutTiming) -> bool
	{
		OutTiming = FWarmTiming();
		for (int32 EntryIndex = 0; EntryIndex < InEntryCount; ++EntryIndex)
		{
			if (!Fixture.WriteFile(FString::Printf(TEXT("Content/__ExternalActors__/Map/A/B/INACTIVE_%d.uasset"), EntryIndex),
				FString::Printf(TEXT("dirty inactive OFPA %d\n"), EntryIndex))) return false;
		}
		TArray<FGitChangedAssetEntry> BatchEntries;
		if (!CaptureRevertEntries(*this, Fixture, PinnedHead, BatchEntries)) return false;
		if (!TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA fixture captures exactly its dirty selection"), InEntryCount), BatchEntries.Num(), InEntryCount)) return false;
		for (FGitChangedAssetEntry& BatchEntry : BatchEntries)
		{
			BatchEntry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
			BatchEntry.OwnerLevel = DestinationOwnerLevel;
			BatchEntry.bOwnerLevelResolved = true;
		}

		FGitChangedAssetRevertLifecycle Lifecycle;
		int32 ConfirmCalls = 0;
		int32 PrepareCalls = 0;
		int32 BeginMutationCalls = 0;
		int32 FinalizeCalls = 0;
		FGitChangedAssetRevertCallbacks Callbacks;
		Callbacks.Confirm = [&Lifecycle, &ConfirmCalls, &OutTiming](const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
		{
			++ConfirmCalls;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bSucceeded = Lifecycle.RecordConfirmedClosure(InEntries, OutError);
			OutTiming.ConfirmMilliseconds += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return bSucceeded;
		};
		Callbacks.PrepareForMutation = [&Lifecycle, &PrepareCalls, &OutTiming](const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
		{
			++PrepareCalls;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bSucceeded = Lifecycle.Prepare(InEntries, OutError);
			OutTiming.PrepareMilliseconds += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return bSucceeded;
		};
		Callbacks.BeginMutation = [&Lifecycle, &BeginMutationCalls, &OutTiming](const TArray<FGitChangedAssetEntry>& InEntries, FString& OutError)
		{
			++BeginMutationCalls;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bSucceeded = Lifecycle.BeginMutation(InEntries, OutError);
			OutTiming.BeginMutationMilliseconds += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return bSucceeded;
		};
		Callbacks.FinalizeEditor = [&Lifecycle, &FinalizeCalls, &OutTiming](const TArray<FGitChangedAssetEntry>& InEntries, const TArray<FString>& InAffectedFiles,
			const EGitChangedAssetMutationOutcome InOutcome, FString& OutError)
		{
			++FinalizeCalls;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bSucceeded = Lifecycle.Finish(InEntries, InAffectedFiles, InOutcome, OutError);
			OutTiming.FinalizeMilliseconds += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			return bSucceeded;
		};
		ReloadBatchCalls = 0;
		FGitChangedAssetRevertResult Result;
		const double TotalStartSeconds = FPlatformTime::Seconds();
		if (!TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert succeeds"), InEntryCount),
			FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(PinnedHead, BatchEntries, Callbacks, Result)))
		{
			OutTiming.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStartSeconds) * 1000.0;
			AddError(FString::Join(Result.Errors, TEXT("\n")));
			return false;
		}
		OutTiming.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStartSeconds) * 1000.0;
		if (!TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert reports disk success"), InEntryCount), Result.bSucceeded)
			|| !TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert reports Editor finalization success"), InEntryCount), Result.bReloadSucceeded)
			|| !TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert confirms once"), InEntryCount), ConfirmCalls, 1)
			|| !TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert prepares once"), InEntryCount), PrepareCalls, 1)
			|| !TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert enters the mutation point once"), InEntryCount), BeginMutationCalls, 1)
			|| !TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert finalizes once"), InEntryCount), FinalizeCalls, 1)
			|| !TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert invokes no package reload batch"), InEntryCount), ReloadBatchCalls, 0)) return false;

		for (int32 EntryIndex = 0; EntryIndex < InEntryCount; ++EntryIndex)
		{
			FString Contents;
			if (!TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert restores worktree file %d"), InEntryCount, EntryIndex),
				Fixture.ReadFile(FString::Printf(TEXT("Content/__ExternalActors__/Map/A/B/INACTIVE_%d.uasset"), EntryIndex), Contents))) return false;
			if (!TestEqual(FString::Printf(TEXT("The %d-asset inactive OFPA Revert restores HEAD bytes for file %d"), InEntryCount, EntryIndex),
				Contents, FString::Printf(TEXT("head inactive OFPA %d\n"), EntryIndex))) return false;
		}
		FString Status;
		if (!Fixture.RunGit(TEXT("status --porcelain=v2"), Status)) return false;
		Status.TrimStartAndEndInline();
		if (!TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert restores worktree and index"), InEntryCount), Status.IsEmpty())) return false;
		FGitChangedAssetSnapshot CleanSnapshot;
		FString SnapshotError;
		if (!TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert recaptures Changed Assets status"), InEntryCount),
			FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, CleanSnapshot, SnapshotError)))
		{
			AddError(SnapshotError);
			return false;
		}
		return TestTrue(FString::Printf(TEXT("The %d-asset inactive OFPA Revert leaves an empty Changed Assets snapshot"), InEntryCount), CleanSnapshot.Entries.IsEmpty());
	};
	for (const int32 EntryCount : { 1, 10, 100 })
	{
		TArray<double> TotalMilliseconds;
		TArray<double> ConfirmMilliseconds;
		TArray<double> PrepareMilliseconds;
		TArray<double> BeginMutationMilliseconds;
		TArray<double> FinalizeMilliseconds;
		TotalMilliseconds.Reserve(3);
		ConfirmMilliseconds.Reserve(3);
		PrepareMilliseconds.Reserve(3);
		BeginMutationMilliseconds.Reserve(3);
		FinalizeMilliseconds.Reserve(3);
		for (int32 Iteration = 1; Iteration <= 3; ++Iteration)
		{
			FWarmTiming Timing;
			if (!RevertInactiveBatch(EntryCount, Timing)) return false;
			AddInfo(FString::Printf(TEXT("OFPA_WARM_TIMING n=%d iteration=%d total_ms=%.3f confirm_ms=%.3f prepare_ms=%.3f begin_mutation_ms=%.3f finalize_ms=%.3f"),
				EntryCount, Iteration, Timing.TotalMilliseconds, Timing.ConfirmMilliseconds, Timing.PrepareMilliseconds,
				Timing.BeginMutationMilliseconds, Timing.FinalizeMilliseconds));
			TotalMilliseconds.Add(Timing.TotalMilliseconds);
			ConfirmMilliseconds.Add(Timing.ConfirmMilliseconds);
			PrepareMilliseconds.Add(Timing.PrepareMilliseconds);
			BeginMutationMilliseconds.Add(Timing.BeginMutationMilliseconds);
			FinalizeMilliseconds.Add(Timing.FinalizeMilliseconds);
		}
		TotalMilliseconds.Sort();
		ConfirmMilliseconds.Sort();
		PrepareMilliseconds.Sort();
		BeginMutationMilliseconds.Sort();
		FinalizeMilliseconds.Sort();
		AddInfo(FString::Printf(TEXT("OFPA_WARM_TIMING_MEDIAN n=%d repeats=3 total_ms=%.3f confirm_ms=%.3f prepare_ms=%.3f begin_mutation_ms=%.3f finalize_ms=%.3f"),
			EntryCount, TotalMilliseconds[1], ConfirmMilliseconds[1], PrepareMilliseconds[1], BeginMutationMilliseconds[1], FinalizeMilliseconds[1]));
	}

	FGitChangedAssetEntry RenameEntry = Entry;
	RenameEntry.State = EGitChangedAssetState::Renamed;
	RenameEntry.IndexStatus = TEXT('R');
	const FString SourceExternalPackageName = TEXT("/Game/__ExternalActors__/") + FPackageName::GetShortName(SourceOwnerLevel) + TEXT("/A/B/INACTIVE_OLD");
	RenameEntry.RenameFromRepositoryRelativePath = TEXT("Content/__ExternalActors__/") + FPackageName::GetShortName(SourceOwnerLevel) + TEXT("/A/B/INACTIVE_OLD.uasset");
	RenameEntry.RenameFromAbsoluteFilename = FPackageName::LongPackageNameToFilename(SourceExternalPackageName, FPackageName::GetAssetPackageExtension());
	if (!TestTrue(TEXT("An external rename source uses the same inactive disk-only lifecycle matcher"),
		FGitChangedAssetRevertLifecycle::BuildPreview({ RenameEntry }, Preview, Error))) return false;
	TestTrue(TEXT("The external rename source does not reload an inactive owner map"), Preview.OwnerMapsToReload.IsEmpty());

	InactiveDestinationOwnerPackage->SetDirtyFlag(true);
	TestFalse(TEXT("An inactive dirty OFPA owner blocks before disk mutation"), FGitChangedAssetRevertLifecycle::BuildPreview({ Entry }, Preview, Error));
	InactiveDestinationOwnerPackage->SetDirtyFlag(false);
	if (!TestTrue(TEXT("The inactive destination owner diagnostic identifies the safety gate"), Error.Contains(TEXT("inactive OFPA owner"), ESearchCase::IgnoreCase))) return false;

	InactiveSourceOwnerPackage->SetDirtyFlag(true);
	TestFalse(TEXT("An inactive dirty rename source owner blocks before disk mutation"), FGitChangedAssetRevertLifecycle::BuildPreview({ RenameEntry }, Preview, Error));
	InactiveSourceOwnerPackage->SetDirtyFlag(false);
	return TestTrue(TEXT("The inactive dirty rename source diagnostic identifies the source safety gate"), Error.Contains(TEXT("inactive OFPA owner"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetReloadFailureDiagnosticAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.ReloadFailureDiagnostic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetReloadFailureDiagnosticAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/ReloadFailure.uasset"), TEXT("head bytes\n")) || !Fixture.CommitAll(TEXT("Reload failure diagnostic fixture"))) return false;
	FString PinnedHead;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), PinnedHead)) return false;
	PinnedHead.TrimStartAndEndInline();
	if (!Fixture.WriteFile(TEXT("Content/ReloadFailure.uasset"), TEXT("dirty bytes\n"))) return false;

	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureRevertEntries(*this, Fixture, PinnedHead, Entries)) return false;
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 BeginMutationCalls = 0;
	int32 FinalizeCalls = 0;
	FGitChangedAssetRevertCallbacks Callbacks = MakeCallbacks(ConfirmCalls, PrepareCalls, BeginMutationCalls, FinalizeCalls);
	Callbacks.FinalizeEditor = [&FinalizeCalls](const TArray<FGitChangedAssetEntry>&, const TArray<FString>&,
		const EGitChangedAssetMutationOutcome, FString& OutError)
	{
		++FinalizeCalls;
		OutError = TEXT("Intentional reload diagnostic seam.");
		return false;
	};
	FGitChangedAssetRevertResult Result;
	const bool bDiskMutationSucceeded = FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(PinnedHead, Entries, Callbacks, Result);
	TestTrue(TEXT("A reload failure after the commit point preserves disk mutation success"), bDiskMutationSucceeded && Result.bSucceeded);
	TestFalse(TEXT("A reload failure is surfaced independently from disk mutation"), Result.bReloadSucceeded);
	TestEqual(TEXT("The reload failure finalizer runs exactly once"), FinalizeCalls, 1);
	return TestTrue(TEXT("The reload failure retains its diagnostic"), Result.Errors.ContainsByPredicate([](const FString& Message)
	{
		return Message.Contains(TEXT("Intentional reload diagnostic seam."), ESearchCase::CaseSensitive);
	}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetStreamingOfpaDirtySiblingAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.StreamingOfpaDirtySibling", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetStreamingOfpaDirtySiblingAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const EditorWorldPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsLifecycle_%s"), *TestId));
	UPackage* const StreamingPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsStreaming_%s"), *TestId));
	if (!TestNotNull(TEXT("Creates an isolated current Editor world package"), EditorWorldPackage) ||
		!TestNotNull(TEXT("Creates an isolated streaming level package"), StreamingPackage)) return false;
	const FName EditorWorldName(*FString::Printf(TEXT("EditorWorld_%s"), *TestId));
	const FName StreamingWorldName(*FString::Printf(TEXT("StreamingWorld_%s"), *TestId));
	UWorld* const EditorWorld = UWorld::CreateWorld(EWorldType::Editor, false, EditorWorldName, EditorWorldPackage);
	UWorld* const StreamingOuterWorld = UWorld::CreateWorld(EWorldType::Inactive, false, StreamingWorldName, StreamingPackage);
	if (!TestNotNull(TEXT("Creates an isolated Editor world"), EditorWorld) || !TestNotNull(TEXT("Creates an isolated streaming world"), StreamingOuterWorld)) return false;
	ULevel* const StreamingLevel = StreamingOuterWorld->PersistentLevel;
	if (!TestNotNull(TEXT("Creates an isolated streaming level"), StreamingLevel)) return false;
	StreamingLevel->OwningWorld = EditorWorld;
	EditorWorld->AddLevel(StreamingLevel);
	StreamingPackage->SetDirtyFlag(false);

	const TArray<FString> ExternalActorPaths = ULevel::GetExternalActorsPaths(StreamingPackage->GetName());
	if (!TestTrue(TEXT("The isolated streaming level has an external-actor root"), !ExternalActorPaths.IsEmpty())) return false;
	const FString SelectedExternalPackageName = ExternalActorPaths[0] + TEXT("/A/B/SELECTED");
	const FString SiblingExternalPackageName = ExternalActorPaths[0] + TEXT("/A/B/SIBLING");
	UPackage* const SelectedExternalPackage = CreatePackage(*SelectedExternalPackageName);
	UPackage* const SiblingExternalPackage = CreatePackage(*SiblingExternalPackageName);
	if (!TestNotNull(TEXT("Creates the selected external package"), SelectedExternalPackage) ||
		!TestNotNull(TEXT("Creates the dirty sibling external package"), SiblingExternalPackage)) return false;
	SelectedExternalPackage->SetDirtyFlag(false);

	const FName DirtySiblingName(*FString::Printf(TEXT("DirtySibling_%s"), *TestId));
	AActor* const DirtySiblingActor = NewObject<AActor>(StreamingLevel, DirtySiblingName, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("Creates a dirty external sibling actor"), DirtySiblingActor)) return false;
	StreamingLevel->Actors.Add(DirtySiblingActor);
	DirtySiblingActor->SetPackageExternal(true, false, SiblingExternalPackage);
	SiblingExternalPackage->SetDirtyFlag(true);
	if (!TestTrue(TEXT("The sibling is discoverable as the dirty external package asset"),
		SiblingExternalPackage->FindAssetInPackage() == DirtySiblingActor && DirtySiblingActor->IsPackageExternal()))
	{
		SiblingExternalPackage->SetDirtyFlag(false);
		return false;
	}

	FGitChangedAssetEntry SelectedEntry;
	SelectedEntry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Test/Selected.uasset");
	SelectedEntry.AbsoluteFilename = FPackageName::LongPackageNameToFilename(SelectedExternalPackageName, FPackageName::GetAssetPackageExtension());
	SelectedEntry.PackageName = SelectedExternalPackageName;
	SelectedEntry.OwnerLevel = StreamingPackage->GetName();
	SelectedEntry.State = EGitChangedAssetState::Modified;
	SelectedEntry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
	SelectedEntry.bBaseRevertEligible = true;
	SelectedEntry.bCanRevert = true;
	SelectedEntry.bMetadataResolved = true;
	SelectedEntry.bOwnerLevelResolved = true;
	SelectedEntry.IndexStatus = TEXT('M');
	SelectedEntry.WorktreeStatus = TEXT('M');

	FScopedCurrentEditorWorldOverride EditorWorldOverride(EditorWorld);
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	TestFalse(TEXT("A dirty non-selected sibling in the matched streaming level blocks the reload closure"),
		FGitChangedAssetRevertLifecycle::BuildPreview({ SelectedEntry }, Preview, Error));
	SiblingExternalPackage->SetDirtyFlag(false);
	return TestTrue(TEXT("The streaming dirty sibling diagnostic identifies the reload closure"),
		Error.Contains(TEXT("owner level reload closure"), ESearchCase::CaseSensitive));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetDirectEditorWorldOfpaAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.DirectEditorWorldOfpa", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetDirectEditorWorldOfpaAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const CurrentWorldPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsCurrent_%s"), *TestId));
	UPackage* const DirectOwnerPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsDirectOwner_%s"), *TestId));
	if (!TestNotNull(TEXT("Creates an isolated current world package"), CurrentWorldPackage) ||
		!TestNotNull(TEXT("Creates an isolated direct owner package"), DirectOwnerPackage)) return false;
	const FName CurrentWorldName(*FString::Printf(TEXT("CurrentWorld_%s"), *TestId));
	const FName DirectWorldName(*FString::Printf(TEXT("DirectWorld_%s"), *TestId));
	UWorld* const CurrentWorld = UWorld::CreateWorld(EWorldType::Editor, false, CurrentWorldName, CurrentWorldPackage);
	UWorld* const DirectOwnerWorld = UWorld::CreateWorld(EWorldType::Editor, false, DirectWorldName, DirectOwnerPackage);
	if (!TestNotNull(TEXT("Creates an isolated current Editor world"), CurrentWorld) ||
		!TestNotNull(TEXT("Creates an isolated direct owner Editor world"), DirectOwnerWorld)) return false;

	const TArray<FString> ExternalActorPaths = ULevel::GetExternalActorsPaths(DirectOwnerPackage->GetName());
	if (!TestTrue(TEXT("The direct owner world has an external-actor root"), !ExternalActorPaths.IsEmpty())) return false;
	const FString ExternalPackageName = ExternalActorPaths[0] + TEXT("/A/B/DIRECT");
	UPackage* const ExternalPackage = CreatePackage(*ExternalPackageName);
	if (!TestNotNull(TEXT("Creates the direct external package"), ExternalPackage)) return false;
	const FName DirectActorName(*FString::Printf(TEXT("DirectActor_%s"), *TestId));
	AActor* const DirectActor = NewObject<AActor>(DirectOwnerWorld->PersistentLevel, DirectActorName, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("Creates the direct external actor"), DirectActor)) return false;
	DirectOwnerWorld->PersistentLevel->Actors.Add(DirectActor);
	DirectActor->SetPackageExternal(true, false, ExternalPackage);
	ExternalPackage->SetDirtyFlag(false);
	DirectOwnerPackage->SetDirtyFlag(false);
	if (!TestTrue(TEXT("The direct external actor is discoverable from its package"),
		ExternalPackage->FindAssetInPackage() == DirectActor && DirectActor->IsPackageExternal()))
	{
		ExternalPackage->SetDirtyFlag(false);
		DirectOwnerPackage->SetDirtyFlag(false);
		return false;
	}

	FGitChangedAssetEntry Entry;
	Entry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Test/Direct.uasset");
	Entry.AbsoluteFilename = FPackageName::LongPackageNameToFilename(ExternalPackageName, FPackageName::GetAssetPackageExtension());
	Entry.PackageName = ExternalPackageName;
	Entry.OwnerLevel = DirectOwnerPackage->GetName();
	Entry.State = EGitChangedAssetState::Modified;
	Entry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
	Entry.bBaseRevertEligible = true;
	Entry.bCanRevert = true;
	Entry.bMetadataResolved = true;
	Entry.bOwnerLevelResolved = true;
	Entry.IndexStatus = TEXT('M');
	Entry.WorktreeStatus = TEXT('M');

	FScopedCurrentEditorWorldOverride CurrentWorldOverride(CurrentWorld);
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	if (!TestTrue(TEXT("A direct non-current Editor-world OFPA builds a reload closure"),
		FGitChangedAssetRevertLifecycle::BuildPreview({ Entry }, Preview, Error))) return false;
	if (!TestEqual(TEXT("The direct OFPA has one owner world reload target"), Preview.OwnerMapsToReload.Num(), 1)) return false;
	return TestEqual(TEXT("The direct OFPA reloads its metadata-matching owner world package"), Preview.OwnerMapsToReload[0], DirectOwnerPackage->GetName());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetOfpaReloadBatchLifecycleAutomationTest, "Cthulhu.GitSourceControl.ChangedAssets.OfpaReloadBatchLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetOfpaReloadBatchLifecycleAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	const FString ContentDirectory = FPaths::Combine(Fixture.GetRoot(), TEXT("Content"));
	if (!TestTrue(TEXT("Creates an isolated mounted content directory"), IFileManager::Get().MakeDirectory(*ContentDirectory, true))) return false;
	const FString MountRoot = FString::Printf(TEXT("/GitChangedAssetsBatch_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory); };

	auto CreateExternalActorEntry = [this, &MountRoot](const FString& InOwnerSuffix, const FString& InExternalSuffix,
		UPackage*& OutOwnerPackage, UWorld*& OutOwnerWorld) -> TOptional<FGitChangedAssetEntry>
	{
		const FString OwnerPackageName = MountRoot + InOwnerSuffix;
		if (OutOwnerPackage == nullptr || OutOwnerWorld == nullptr)
		{
			const FString OwnerFilename = FPackageName::LongPackageNameToFilename(OwnerPackageName, FPackageName::GetMapPackageExtension());
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(OwnerFilename), true)
				|| !FFileHelper::SaveStringToFile(TEXT("Changed Assets OFPA batch lifecycle fixture\n"), *OwnerFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				AddError(FString::Printf(TEXT("Could not create a mounted owner map fixture: %s"), *OwnerFilename));
				return {};
			}
			OutOwnerPackage = CreatePackage(*OwnerPackageName);
			OutOwnerWorld = OutOwnerPackage != nullptr
				? UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("BatchOwner_%s"), *InOwnerSuffix)), OutOwnerPackage)
				: nullptr;
			if (OutOwnerPackage == nullptr || OutOwnerWorld == nullptr)
			{
				AddError(FString::Printf(TEXT("Could not create an isolated OFPA owner world: %s"), *OwnerPackageName));
				return {};
			}
		}
		const TArray<FString> ExternalActorPaths = ULevel::GetExternalActorsPaths(OwnerPackageName);
		if (ExternalActorPaths.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Could not derive an external actor path for: %s"), *OwnerPackageName));
			return {};
		}
		const FString ExternalPackageName = ExternalActorPaths[0] + TEXT("/A/B/") + InExternalSuffix;
		UPackage* const ExternalPackage = CreatePackage(*ExternalPackageName);
		AActor* const ExternalActor = ExternalPackage != nullptr
			? NewObject<AActor>(OutOwnerWorld->PersistentLevel, FName(*FString::Printf(TEXT("BatchActor_%s"), *InExternalSuffix)), RF_Public | RF_Standalone)
			: nullptr;
		if (ExternalPackage == nullptr || ExternalActor == nullptr)
		{
			AddError(FString::Printf(TEXT("Could not create an isolated OFPA external package: %s"), *ExternalPackageName));
			return {};
		}
		OutOwnerWorld->PersistentLevel->Actors.Add(ExternalActor);
		ExternalActor->SetPackageExternal(true, false, ExternalPackage);
		ExternalPackage->SetDirtyFlag(false);
		OutOwnerPackage->SetDirtyFlag(false);

		FGitChangedAssetEntry Entry;
		Entry.RepositoryRelativePath = FString::Printf(TEXT("Content/__ExternalActors__/Batch/%s.uasset"), *InExternalSuffix);
		Entry.AbsoluteFilename = FPackageName::LongPackageNameToFilename(ExternalPackageName, FPackageName::GetAssetPackageExtension());
		Entry.PackageName = ExternalPackageName;
		Entry.OwnerLevel = OwnerPackageName;
		Entry.State = EGitChangedAssetState::Modified;
		Entry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
		Entry.bBaseRevertEligible = true;
		Entry.bCanRevert = true;
		Entry.bMetadataResolved = true;
		Entry.bOwnerLevelResolved = true;
		Entry.IndexStatus = TEXT('M');
		Entry.WorktreeStatus = TEXT('M');
		return Entry;
	};

	UPackage* CurrentWorldPackage = CreatePackage(*(MountRoot + TEXT("Current")));
	UWorld* const CurrentWorld = CurrentWorldPackage != nullptr
		? UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("BatchCurrent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))), CurrentWorldPackage)
		: nullptr;
	UPackage* OwnerAPackage = nullptr;
	UWorld* OwnerAWorld = nullptr;
	UPackage* OwnerBPackage = nullptr;
	UWorld* OwnerBWorld = nullptr;
	TOptional<FGitChangedAssetEntry> OwnerAFirst = CreateExternalActorEntry(TEXT("OwnerA"), TEXT("OwnerAFirst"), OwnerAPackage, OwnerAWorld);
	TOptional<FGitChangedAssetEntry> OwnerASecond = CreateExternalActorEntry(TEXT("OwnerA"), TEXT("OwnerASecond"), OwnerAPackage, OwnerAWorld);
	TOptional<FGitChangedAssetEntry> OwnerB = CreateExternalActorEntry(TEXT("OwnerB"), TEXT("OwnerB"), OwnerBPackage, OwnerBWorld);
	if (!TestNotNull(TEXT("Creates an isolated current Editor world"), CurrentWorld)
		|| !TestNotNull(TEXT("Creates the first OFPA owner world"), OwnerAWorld)
		|| !TestNotNull(TEXT("Creates the second OFPA owner world"), OwnerBWorld)
		|| !TestTrue(TEXT("Creates two external entries for one owner and one for another"), OwnerAFirst.IsSet() && OwnerASecond.IsSet() && OwnerB.IsSet())) return false;
	CurrentWorldPackage->SetDirtyFlag(false);
	OwnerAPackage->SetDirtyFlag(false);
	OwnerBPackage->SetDirtyFlag(false);

	const TArray<FGitChangedAssetEntry> Entries = { OwnerAFirst.GetValue(), OwnerASecond.GetValue(), OwnerB.GetValue() };
	FScopedCurrentEditorWorldOverride CurrentWorldOverride(CurrentWorld);
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	if (!TestTrue(TEXT("Multiple OFPA owners build a reload closure"), FGitChangedAssetRevertLifecycle::BuildPreview(Entries, Preview, Error))) return false;
	if (!TestEqual(TEXT("Two OFPA owners produce two unique owner packages"), Preview.OwnerMapsToReload.Num(), 2)) return false;
	TestTrue(TEXT("The shared owner appears once in the lifecycle closure"), Preview.OwnerMapsToReload.Contains(OwnerAPackage->GetName()));
	TestTrue(TEXT("The second owner appears in the lifecycle closure"), Preview.OwnerMapsToReload.Contains(OwnerBPackage->GetName()));

	TArray<TArray<FString>> ReloadBatches;
	FGitChangedAssetRevertLifecycle::SetReloadPackagesForTesting([&ReloadBatches](const TArray<UPackage*>& InPackages, FString&)
	{
		TArray<FString> PackageNames;
		for (UPackage* Package : InPackages)
		{
			if (Package != nullptr)
			{
				PackageNames.Add(Package->GetName());
			}
		}
		ReloadBatches.Add(MoveTemp(PackageNames));
		return true;
	});
	ON_SCOPE_EXIT { FGitChangedAssetRevertLifecycle::SetReloadPackagesForTesting({}); };
	FGitChangedAssetRevertLifecycle Lifecycle;
	if (!TestTrue(TEXT("The OFPA batch closure can be recorded"), Lifecycle.RecordConfirmedClosure(Entries, Error))) return false;
	if (!TestTrue(TEXT("The OFPA batch closure prepares unique owner packages"), Lifecycle.Prepare(Entries, Error))) return false;
	if (!TestTrue(TEXT("The OFPA batch closure finalizes through the reload boundary"),
		Lifecycle.Finish(Entries, {}, EGitChangedAssetMutationOutcome::Succeeded, Error))) return false;
	if (!TestEqual(TEXT("All OFPA owner worlds are submitted through one ReloadPackages batch"), ReloadBatches.Num(), 1)) return false;
	if (!TestEqual(TEXT("The ReloadPackages batch contains two unique owner maps"), ReloadBatches[0].Num(), 2)) return false;
	TestTrue(TEXT("The ReloadPackages batch contains the shared owner once"), ReloadBatches[0].Contains(OwnerAPackage->GetName()));
	return TestTrue(TEXT("The ReloadPackages batch contains the second owner"), ReloadBatches[0].Contains(OwnerBPackage->GetName()));
}

#endif
