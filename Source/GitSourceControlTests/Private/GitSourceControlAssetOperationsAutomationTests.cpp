// Copyright (c) 2026

#include "GitSourceControlAssetOperations.h"

#include "Curves/CurveFloat.h"
#include "EditorValidatorSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlAssetOperationsAutomationTestsPrivate
{
	FString QuoteGitArgument(const FString& Argument)
	{
		FString Escaped = Argument;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	FString NormalizeDirectory(const FString& Directory)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	bool IsSameOrUnderDirectory(const FString& Path, const FString& Directory)
	{
		const FString NormalizedPath = NormalizeDirectory(Path);
		const FString NormalizedDirectory = NormalizeDirectory(Directory);
		return FPaths::IsSamePath(NormalizedPath, NormalizedDirectory) || FPaths::IsUnderDirectory(NormalizedPath, NormalizedDirectory);
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
				Test.AddError(TEXT("Git executable is required for asset-operation automation tests."));
				return false;
			}
			Root = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitSourceControlAssetOperationTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IsSafePath() || !IFileManager::Get().MakeDirectory(*Root, true))
			{
				Test.AddError(FString::Printf(TEXT("Could not create safe asset-operation fixture: %s"), *Root));
				return false;
			}
			return RunGit(TEXT("init")) && RunGit(TEXT("config user.name \"GitSourceControlTests\"")) && RunGit(TEXT("config user.email \"git-source-control-tests@example.invalid\""));
		}

		bool WriteFile(const FString& RelativeFilename, const FString& Contents) const
		{
			const FString Filename = AbsoluteFilename(RelativeFilename);
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true) && FFileHelper::SaveStringToFile(Contents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool ReadFile(const FString& RelativeFilename, FString& OutContents) const { return FFileHelper::LoadFileToString(OutContents, *AbsoluteFilename(RelativeFilename)); }
		bool RunGit(const FString& Arguments, FString& OutOutput) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *Arguments);
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutOutput, &StandardError);
			if (ReturnCode == 0) return true;
			Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
			return false;
		}
		bool RunGit(const FString& Arguments) const { FString Output; return RunGit(Arguments, Output); }
		bool CommitAll(const FString& Message) const { return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(Message))); }
		FString AbsoluteFilename(const FString& RelativeFilename) const { return FPaths::Combine(Root, RelativeFilename); }
		const FString& GetGitBinary() const { return GitBinary; }
		const FString& GetRoot() const { return Root; }

	private:
		bool IsSafePath() const
		{
			const FString Parent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitSourceControlAssetOperationTests"));
			if (!IsSameOrUnderDirectory(Root, Parent) || IsSameOrUnderDirectory(Root, FPaths::ProjectDir())) return false;
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (IsSameOrUnderDirectory(Root, Plugin->GetBaseDir())) return false;
			}
			return true;
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString Root;
	};

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakeAcceptingCallbacks(int32& ConfirmCalls, int32& PrepareCalls, int32& ReloadCalls)
	{
		using namespace GitSourceControlAssetOperations;
		FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [&ConfirmCalls](const FString&, const TArray<FString>&) { ++ConfirmCalls; return true; };
		Callbacks.PrepareForMutation = [&PrepareCalls](const TArray<FString>&) { ++PrepareCalls; return true; };
		Callbacks.ReloadPackages = [&ReloadCalls](const TArray<FString>&) { ++ReloadCalls; return true; };
		return Callbacks;
	}

	bool TestCallbacks(FAutomationTestBase& Test, int32 ConfirmCalls, int32 PrepareCalls, int32 ReloadCalls)
	{
		return Test.TestEqual(TEXT("Confirm callback runs once"), ConfirmCalls, 1)
			&& Test.TestEqual(TEXT("Prepare callback runs once"), PrepareCalls, 1)
			&& Test.TestEqual(TEXT("Reload callback runs once"), ReloadCalls, 1);
	}

	bool SaveCurvePackage(FAutomationTestBase& Test, UPackage* Package, UCurveFloat* Asset, const FString& Filename, const FString& Revision)
	{
		FMetaData& Metadata = Package->GetMetaData();
		Metadata.SetValue(Asset, TEXT("GitSourceControlAssetOperationRevision"), *Revision);
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		return Test.TestTrue(*FString::Printf(TEXT("Saved curve package revision %s"), *Revision), UPackage::SavePackage(Package, Asset, *Filename, SaveArgs));
	}

	bool IsGitLfsAvailable(const FString& GitBinary)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*GitBinary, TEXT("lfs version"), &ReturnCode, &StandardOutput, &StandardError);
		return ReturnCode == 0;
	}

	bool ParseLfsPointer(const FString& Pointer, FString& OutOid, int64& OutSize)
	{
		OutOid.Reset();
		OutSize = 0;
		TArray<FString> Lines;
		Pointer.ParseIntoArrayLines(Lines, false);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardAutomationTest, "Cthulhu.GitSourceControl.AssetOperations.Discard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("head bytes\n")) || !Fixture.CommitAll(TEXT("Initial tracked uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("staged bytes\n")) || !Fixture.RunGit(TEXT("add -- Content/Tracked.uasset")) || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("unstaged bytes\n"))) return false;
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	const FString Filename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	FString IndexBeforeCommitPoint;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexBeforeCommitPoint)) return false;
	const FString ExternalWorktreeContents = TEXT("commit-point changed asset\n");
	int32 RaceConfirmCalls = 0;
	int32 RacePrepareCalls = 0;
	int32 RaceReloadCalls = 0;
	FGitAssetOperationCallbacks RaceCallbacks = MakeAcceptingCallbacks(RaceConfirmCalls, RacePrepareCalls, RaceReloadCalls);
	RaceCallbacks.BeforeCommitPointForTesting = [&Fixture, &ExternalWorktreeContents]()
	{
		Fixture.WriteFile(TEXT("Content/Tracked.uasset"), ExternalWorktreeContents);
	};
	FGitAssetOperationResult RaceResult;
	TestFalse(TEXT("Discard rejects an asset changed after Confirm and Prepare"), Operations.DiscardTrackedFiles({ Filename }, RaceCallbacks, RaceResult));
	TestFalse(TEXT("Discard commit-point rejection does not report success"), RaceResult.bSucceeded);
	FString WorktreeAfterCommitPoint;
	FString IndexAfterCommitPoint;
	TestTrue(TEXT("Discard commit-point rejection keeps the external worktree bytes"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), WorktreeAfterCommitPoint));
	TestEqual(TEXT("Discard commit-point rejection preserves the callback write"), WorktreeAfterCommitPoint, ExternalWorktreeContents);
	TestTrue(TEXT("Discard commit-point rejection keeps index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterCommitPoint));
	TestEqual(TEXT("Discard commit-point rejection keeps index unchanged"), IndexAfterCommitPoint, IndexBeforeCommitPoint);
	if (!TestCallbacks(*this, RaceConfirmCalls, RacePrepareCalls, RaceReloadCalls)) return false;

	FString IndexBeforeInjectedFailure;
	FString WorktreeBeforeInjectedFailure;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexBeforeInjectedFailure)
		|| !Fixture.ReadFile(TEXT("Content/Tracked.uasset"), WorktreeBeforeInjectedFailure)) return false;
	int32 FailedMutationConfirmCalls = 0;
	int32 FailedMutationPrepareCalls = 0;
	int32 FailedMutationReloadCalls = 0;
	FGitAssetOperationCallbacks FailedMutationCallbacks = MakeAcceptingCallbacks(FailedMutationConfirmCalls, FailedMutationPrepareCalls, FailedMutationReloadCalls);
	FailedMutationCallbacks.AllowGitMutationForTesting = [](const FString& GitSubcommand)
	{
		return !GitSubcommand.Equals(TEXT("restore"), ESearchCase::CaseSensitive);
	};
	FailedMutationCallbacks.ReloadPackages = [&FailedMutationReloadCalls](const TArray<FString>&)
	{
		++FailedMutationReloadCalls;
		return false;
	};
	FGitAssetOperationResult FailedMutationResult;
	TestFalse(TEXT("Discard reports the injected Git restore failure"), Operations.DiscardTrackedFiles({ Filename }, FailedMutationCallbacks, FailedMutationResult));
	TestFalse(TEXT("Injected Git restore failure does not report success"), FailedMutationResult.bSucceeded);
	TestFalse(TEXT("Injected Git restore failure reports failed package recovery"), FailedMutationResult.bReloadSucceeded);
	TestTrue(TEXT("Injected Git restore failure records the package recovery diagnostic"), FailedMutationResult.Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("reload callback failed"), ESearchCase::IgnoreCase);
	}));
	FString IndexAfterInjectedFailure;
	FString WorktreeAfterInjectedFailure;
	TestTrue(TEXT("Injected Git restore failure keeps index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterInjectedFailure));
	TestEqual(TEXT("Injected Git restore failure rolls back the index"), IndexAfterInjectedFailure, IndexBeforeInjectedFailure);
	TestTrue(TEXT("Injected Git restore failure keeps worktree readable"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), WorktreeAfterInjectedFailure));
	TestEqual(TEXT("Injected Git restore failure rolls back worktree bytes"), WorktreeAfterInjectedFailure, WorktreeBeforeInjectedFailure);
	if (!TestCallbacks(*this, FailedMutationConfirmCalls, FailedMutationPrepareCalls, FailedMutationReloadCalls)) return false;

	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Tracked uasset discard succeeds"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	FString Contents;
	TestTrue(TEXT("Discarded asset is readable"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), Contents));
	TestEqual(TEXT("Discard restores HEAD worktree bytes"), Contents, FString(TEXT("head bytes\n")));
	FString Status;
	TestTrue(TEXT("Discard leaves the asset status readable"), Fixture.RunGit(TEXT("status --porcelain=v2 -- Content/Tracked.uasset"), Status));
	Status.TrimStartAndEndInline();
	TestTrue(TEXT("Discard leaves no selected asset residue"), Status.IsEmpty());
	return Result.bSucceeded && TestCallbacks(*this, ConfirmCalls, PrepareCalls, ReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardLfsAutomationTest, "Cthulhu.GitSourceControl.AssetOperations.DiscardLfs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardLfsAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	if (!IsGitLfsAvailable(Fixture.GetGitBinary()))
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping DiscardLfs."));
		return true;
	}
	if (!Fixture.RunGit(TEXT("lfs install --local"))
		|| !Fixture.RunGit(TEXT("lfs track \"Content/*.uasset\""))
		|| !Fixture.WriteFile(TEXT(".gitignore"), TEXT("Origin.git/\n"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("head LFS bytes\n"))
		|| !Fixture.CommitAll(TEXT("Initial LFS tracked asset")))
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
	FString PointerText;
	if (!Fixture.RunGit(TEXT("show HEAD:Content/Tracked.uasset"), PointerText)) return false;
	FString Oid;
	int64 Size = 0;
	if (!TestTrue(TEXT("Discard LFS fixture pointer parses"), ParseLfsPointer(PointerText, Oid, Size))) return false;
	const FString ObjectFilename = FPaths::Combine(Fixture.GetRoot(), TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
	FString VerifyError;
	if (!TestTrue(TEXT("Discard LFS fixture cache object verifies"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetRoot(), ObjectFilename, Oid, Size, VerifyError)))
	{
		AddError(VerifyError);
		return false;
	}
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	const FString Filename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	if (!Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("staged LFS bytes\n")) || !Fixture.RunGit(TEXT("add -- Content/Tracked.uasset")) || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("unstaged LFS bytes\n"))) return false;
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationResult Result;
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	if (!TestTrue(TEXT("Discard LFS cache hit succeeds"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	TestEqual(TEXT("Discard LFS cache hit starts no fetch"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(0));
	FString Contents;
	TestTrue(TEXT("Discard LFS cache hit restores HEAD content"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), Contents));
	TestEqual(TEXT("Discard LFS cache hit content matches HEAD"), Contents, FString(TEXT("head LFS bytes\n")));
	if (!TestCallbacks(*this, ConfirmCalls, PrepareCalls, ReloadCalls) || !TestTrue(TEXT("Discard LFS removes cache object to force a miss"), IFileManager::Get().Delete(*ObjectFilename, false, true, true))) return false;
	if (!Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("staged LFS miss bytes\n")) || !Fixture.RunGit(TEXT("add -- Content/Tracked.uasset")) || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("unstaged LFS miss bytes\n"))) return false;
	ConfirmCalls = 0;
	PrepareCalls = 0;
	ReloadCalls = 0;
	Result = FGitAssetOperationResult();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	if (!TestTrue(TEXT("Discard LFS cache miss succeeds"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	TestEqual(TEXT("Discard LFS cache miss performs one targeted fetch"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(1));
	TestTrue(TEXT("Discard LFS cache miss verifies downloaded object"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetRoot(), ObjectFilename, Oid, Size, VerifyError));
	TestTrue(TEXT("Discard LFS cache miss restores HEAD content"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), Contents));
	TestEqual(TEXT("Discard LFS cache miss content matches HEAD"), Contents, FString(TEXT("head LFS bytes\n")));
	if (!TestCallbacks(*this, ConfirmCalls, PrepareCalls, ReloadCalls)) return false;

	// A real LFS fetch failure must stop before Editor preparation or any mutation.
	if (!TestTrue(TEXT("Discard LFS failure removes the cached object"), IFileManager::Get().Delete(*ObjectFilename, false, true, true))
		|| !Fixture.RunGit(FString::Printf(TEXT("remote set-url origin %s"), *QuoteGitArgument(Fixture.AbsoluteFilename(TEXT("MissingOrigin.git")))))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("staged LFS failure bytes\n"))
		|| !Fixture.RunGit(TEXT("add -- Content/Tracked.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("unstaged LFS failure bytes\n"))) return false;
	ConfirmCalls = 0;
	PrepareCalls = 0;
	ReloadCalls = 0;
	Result = FGitAssetOperationResult();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	TestFalse(TEXT("Discard LFS fetch failure stops before mutation"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls), Result));
	TestTrue(TEXT("Discard LFS fetch failure reports an error"), !Result.Errors.IsEmpty());
	TestEqual(TEXT("Discard LFS fetch failure never prepares mutation"), PrepareCalls, 0);
	TestEqual(TEXT("Discard LFS fetch failure never reloads packages"), ReloadCalls, 0);
	TestTrue(TEXT("Discard LFS fetch failure preserves worktree bytes"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), Contents));
	return TestEqual(TEXT("Discard LFS fetch failure keeps dirty worktree unchanged"), Contents, FString(TEXT("unstaged LFS failure bytes\n")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetHistoryRestoreAutomationTest, "Cthulhu.GitSourceControl.AssetOperations.HistoryRestore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetHistoryRestoreAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	FScopedDisableValidateOnSave DisableValidateOnSave;
	const FString ContentDirectory = FPaths::Combine(Fixture.GetRoot(), TEXT("Content"));
	const FString MountRoot = FString::Printf(TEXT("/GitSourceControlAssetRestore_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory); };
	const FString RelativeFilename = TEXT("Content/Tracked.uasset");
	const FString PackageName = MountRoot + TEXT("Tracked");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	UPackage* Package = CreatePackage(*PackageName);
	UCurveFloat* Asset = Package ? NewObject<UCurveFloat>(Package, TEXT("Tracked"), RF_Public | RF_Standalone) : nullptr;
	if (!TestNotNull(TEXT("Restore fixture package exists"), Package) || !TestNotNull(TEXT("Restore fixture asset exists"), Asset)) return false;
	Asset->FloatCurve.AddKey(0.0f, 1.0f);
	if (!SaveCurvePackage(*this, Package, Asset, Filename, TEXT("first")) || !Fixture.CommitAll(TEXT("Initial restorable uasset"))) return false;
	FString FirstCommit;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), FirstCommit)) return false;
	FirstCommit.TrimStartAndEndInline();
	TArray<uint8> FirstBytes;
	if (!TestTrue(TEXT("First package bytes are available"), FFileHelper::LoadFileToArray(FirstBytes, *Filename))) return false;
	Asset->FloatCurve.AddKey(1.0f, 2.0f);
	if (!SaveCurvePackage(*this, Package, Asset, Filename, TEXT("second")) || !Fixture.CommitAll(TEXT("Current uasset revision"))) return false;
	Package->SetDirtyFlag(false);
	TArray<UPackage*> PackagesToUnload;
	PackagesToUnload.Add(Package);
	FText UnloadError;
	if (!TestTrue(TEXT("Current package unloads before direct Restore"), UPackageTools::UnloadPackages(PackagesToUnload, UnloadError))) { AddError(UnloadError.ToString()); return false; }
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexBefore)) return false;
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	const FString ExternalWorktreeContents = TEXT("commit-point changed asset\n");
	int32 RaceConfirmCalls = 0;
	int32 RacePrepareCalls = 0;
	int32 RaceReloadCalls = 0;
	FGitAssetOperationCallbacks RaceCallbacks = MakeAcceptingCallbacks(RaceConfirmCalls, RacePrepareCalls, RaceReloadCalls);
	RaceCallbacks.BeforeCommitPointForTesting = [&Fixture, &ExternalWorktreeContents]()
	{
		Fixture.WriteFile(TEXT("Content/Tracked.uasset"), ExternalWorktreeContents);
	};
	FGitAssetOperationResult RaceResult;
	TestFalse(TEXT("Restore rejects an asset changed after Confirm and Prepare"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, RaceCallbacks, RaceResult));
	TestFalse(TEXT("Restore commit-point rejection does not report success"), RaceResult.bSucceeded);
	TArray<uint8> WorktreeAfterCommitPoint;
	TestTrue(TEXT("Restore commit-point rejection leaves the external worktree bytes readable"), FFileHelper::LoadFileToArray(WorktreeAfterCommitPoint, *Filename));
	FString ExternalWorktreeAfterCommitPoint;
	TestTrue(TEXT("Restore commit-point rejection reads the callback write"), FFileHelper::LoadFileToString(ExternalWorktreeAfterCommitPoint, *Filename));
	TestEqual(TEXT("Restore commit-point rejection preserves the callback write"), ExternalWorktreeAfterCommitPoint, ExternalWorktreeContents);
	FString IndexAfterCommitPoint;
	TestTrue(TEXT("Restore commit-point rejection leaves index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterCommitPoint));
	TestEqual(TEXT("Restore commit-point rejection leaves index unchanged"), IndexAfterCommitPoint, IndexBefore);
	if (!TestCallbacks(*this, RaceConfirmCalls, RacePrepareCalls, RaceReloadCalls)) return false;
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	FGitAssetOperationCallbacks Callbacks = MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls);
	FString ConfirmationDescription;
	Callbacks.Confirm = [&ConfirmCalls, &ConfirmationDescription](const FString& Description, const TArray<FString>&)
	{
		++ConfirmCalls;
		ConfirmationDescription = Description;
		return true;
	};
	Callbacks.ReloadPackages = [&ReloadCalls](const TArray<FString>&)
	{
		++ReloadCalls;
		return false;
	};
	FGitAssetOperationResult Result;
	TestFalse(TEXT("Same-path historical uasset Restore fails when reload reports failure"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, Callbacks, Result));
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("Restored uasset bytes are readable"), FFileHelper::LoadFileToArray(RestoredBytes, *Filename));
	TestTrue(TEXT("Restore writes requested historical uasset bytes"), RestoredBytes == FirstBytes);
	FString IndexAfter;
	TestTrue(TEXT("Index is readable after same-path Restore"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfter));
	TestEqual(TEXT("Same-path Force Restore resets the index to HEAD"), IndexAfter, IndexBefore);
	TestFalse(TEXT("Successful disk Restore does not misreport success after reload failure"), Result.bSucceeded);
	TestFalse(TEXT("Successful disk Restore propagates the reload failure"), Result.bReloadSucceeded);
	TestTrue(TEXT("Successful disk Restore records the reload failure diagnostic"), Result.Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("reload callback failed"), ESearchCase::IgnoreCase);
	}));
	TestTrue(TEXT("Historical Restore confirmation states the selected index reset"), ConfirmationDescription.Contains(TEXT("selected Git index entry will be reset to HEAD"), ESearchCase::CaseSensitive));
	if (!TestCallbacks(*this, ConfirmCalls, PrepareCalls, ReloadCalls)) return false;

	auto ForceRestoreTracked = [&]()
	{
		int32 ForceConfirmCalls = 0;
		int32 ForcePrepareCalls = 0;
		int32 ForceReloadCalls = 0;
		FGitAssetOperationResult ForceResult;
		const bool bRestored = Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename,
			MakeAcceptingCallbacks(ForceConfirmCalls, ForcePrepareCalls, ForceReloadCalls), ForceResult);
		FString ForceIndex;
		TArray<uint8> ForceBytes;
		return bRestored && ForceResult.bSucceeded && ForceResult.bReloadSucceeded &&
			Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), ForceIndex) &&
			FFileHelper::LoadFileToArray(ForceBytes, *Filename) && ForceIndex == IndexBefore && ForceBytes == FirstBytes &&
			ForceConfirmCalls == 1 && ForcePrepareCalls == 1 && ForceReloadCalls == 1;
	};

	if (!FFileHelper::SaveStringToFile(TEXT("force working change\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!TestTrue(TEXT("Force Restore accepts a dirty working tree"), ForceRestoreTracked())) return false;
	if (!FFileHelper::SaveStringToFile(TEXT("force staged change\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!Fixture.RunGit(TEXT("add -- Content/Tracked.uasset")) ||
		!FFileHelper::SaveStringToFile(TEXT("force staged and working change\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!TestTrue(TEXT("Force Restore accepts staged and working changes"), ForceRestoreTracked())) return false;
	if (!Fixture.RunGit(TEXT("rm --cached -- Content/Tracked.uasset")) ||
		!TestTrue(TEXT("Force Restore accepts an untracked worktree file"), ForceRestoreTracked())) return false;

	const FString AddedRelativeFilename = TEXT("Content/ForceAdded.uasset");
	const FString AddedFilename = Fixture.AbsoluteFilename(AddedRelativeFilename);
	if (!FFileHelper::SaveArrayToFile(FirstBytes, *AddedFilename) || !Fixture.RunGit(TEXT("add -- Content/ForceAdded.uasset")) ||
		!Fixture.RunGit(TEXT("commit --no-gpg-sign -m \"Force restore added-path history\" -- Content/ForceAdded.uasset"))) return false;
	FString AddedCommit;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), AddedCommit)) return false;
	AddedCommit.TrimStartAndEndInline();
	if (!IFileManager::Get().Delete(*AddedFilename, false, true, true) || !Fixture.RunGit(TEXT("add --all -- Content/ForceAdded.uasset")) ||
		!Fixture.RunGit(TEXT("commit --no-gpg-sign -m \"Force restore added-path deletion\" -- Content/ForceAdded.uasset")) ||
		!FFileHelper::SaveArrayToFile(FirstBytes, *AddedFilename) || !Fixture.RunGit(TEXT("add -- Content/ForceAdded.uasset"))) return false;
	int32 AddedConfirmCalls = 0;
	int32 AddedPrepareCalls = 0;
	int32 AddedReloadCalls = 0;
	FGitAssetOperationResult AddedResult;
	if (!TestTrue(TEXT("Force Restore accepts an index-added path"), Operations.RestoreRevisionToWorkspace(AddedFilename, AddedCommit, AddedRelativeFilename,
		MakeAcceptingCallbacks(AddedConfirmCalls, AddedPrepareCalls, AddedReloadCalls), AddedResult))) return false;
	FString AddedIndex;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/ForceAdded.uasset"), AddedIndex) ||
		!TestTrue(TEXT("Force Restore resets an index-added path to HEAD absence"), AddedIndex.TrimStartAndEnd().IsEmpty())) return false;
	TArray<uint8> AddedBytes;
	if (!TestTrue(TEXT("Force Restore writes the selected revision for an index-added path"), FFileHelper::LoadFileToArray(AddedBytes, *AddedFilename)) ||
		!TestTrue(TEXT("Index-added Force Restore writes the requested package bytes"), AddedBytes == FirstBytes)) return false;
	AddedResult = FGitAssetOperationResult();
	if (!TestTrue(TEXT("Force Restore accepts an untracked historical path"), Operations.RestoreRevisionToWorkspace(AddedFilename, AddedCommit, AddedRelativeFilename,
		MakeAcceptingCallbacks(AddedConfirmCalls, AddedPrepareCalls, AddedReloadCalls), AddedResult))) return false;
	if (!IFileManager::Get().Delete(*AddedFilename, false, true, true)) return false;
	if (!Fixture.RunGit(TEXT("restore --source=HEAD --staged --worktree -- Content/Tracked.uasset"))) return false;

	FString MainBranch;
	if (!Fixture.RunGit(TEXT("branch --show-current"), MainBranch)) return false;
	MainBranch.TrimStartAndEndInline();
	if (!Fixture.RunGit(FString::Printf(TEXT("branch force-restore-conflict %s"), *FirstCommit)) ||
		!Fixture.RunGit(TEXT("checkout force-restore-conflict")) ||
		!FFileHelper::SaveStringToFile(TEXT("other conflict bytes\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!Fixture.CommitAll(TEXT("Force restore conflict branch")) ||
		!Fixture.RunGit(FString::Printf(TEXT("checkout %s"), *MainBranch))) return false;
	int32 MergeReturnCode = INDEX_NONE;
	FString MergeOutput;
	FString MergeError;
	FPlatformProcess::ExecProcess(*Fixture.GetGitBinary(), *FString::Printf(TEXT("-C %s merge force-restore-conflict"), *QuoteGitArgument(Fixture.GetRoot())), &MergeReturnCode, &MergeOutput, &MergeError);
	if (!TestTrue(TEXT("Force Restore fixture creates a merge conflict"), MergeReturnCode != 0)) return false;
	FString ConflictIndexBeforeRollback;
	TArray<uint8> ConflictBytesBeforeRollback;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), ConflictIndexBeforeRollback) || !FFileHelper::LoadFileToArray(ConflictBytesBeforeRollback, *Filename)) return false;
	int32 ConflictRollbackConfirmCalls = 0;
	int32 ConflictRollbackPrepareCalls = 0;
	int32 ConflictRollbackReloadCalls = 0;
	FGitAssetOperationCallbacks ConflictRollbackCallbacks = MakeAcceptingCallbacks(ConflictRollbackConfirmCalls, ConflictRollbackPrepareCalls, ConflictRollbackReloadCalls);
	ConflictRollbackCallbacks.AllowWorktreeReplaceForTesting = []() { return false; };
	FGitAssetOperationResult ConflictRollbackResult;
	TestFalse(TEXT("Force Restore conflict rollback seam rejects worktree replacement"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, ConflictRollbackCallbacks, ConflictRollbackResult));
	FString ConflictIndexAfterRollback;
	TArray<uint8> ConflictBytesAfterRollback;
	TestTrue(TEXT("Force Restore conflict rollback reads the index"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), ConflictIndexAfterRollback));
	TestEqual(TEXT("Force Restore conflict rollback restores every conflict stage"), ConflictIndexAfterRollback, ConflictIndexBeforeRollback);
	TestTrue(TEXT("Force Restore conflict rollback restores worktree bytes"), FFileHelper::LoadFileToArray(ConflictBytesAfterRollback, *Filename));
	TestTrue(TEXT("Force Restore conflict rollback keeps worktree bytes"), ConflictBytesAfterRollback == ConflictBytesBeforeRollback);
	if (!TestCallbacks(*this, ConflictRollbackConfirmCalls, ConflictRollbackPrepareCalls, ConflictRollbackReloadCalls)) return false;
	if (!TestTrue(TEXT("Force Restore accepts a conflicted index"), ForceRestoreTracked())) return false;
	if (!Fixture.RunGit(TEXT("reset --hard HEAD"))) return false;

	FString RollbackIndexBefore;
	TArray<uint8> RollbackBytesBefore;
	if (!FFileHelper::SaveStringToFile(TEXT("rollback staged bytes\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!Fixture.RunGit(TEXT("add -- Content/Tracked.uasset")) ||
		!FFileHelper::SaveStringToFile(TEXT("rollback working bytes\n"), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), RollbackIndexBefore) || !FFileHelper::LoadFileToArray(RollbackBytesBefore, *Filename)) return false;
	int32 RollbackConfirmCalls = 0;
	int32 RollbackPrepareCalls = 0;
	int32 RollbackReloadCalls = 0;
	FGitAssetOperationCallbacks RollbackCallbacks = MakeAcceptingCallbacks(RollbackConfirmCalls, RollbackPrepareCalls, RollbackReloadCalls);
	RollbackCallbacks.AllowWorktreeReplaceForTesting = []() { return false; };
	FGitAssetOperationResult RollbackResult;
	TestFalse(TEXT("Force Restore rollback seam rejects worktree replacement after index reset"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, RollbackCallbacks, RollbackResult));
	FString RollbackIndexAfter;
	TArray<uint8> RollbackBytesAfter;
	TestTrue(TEXT("Force Restore rollback restores the index snapshot"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), RollbackIndexAfter));
	TestEqual(TEXT("Force Restore rollback keeps the exact index snapshot"), RollbackIndexAfter, RollbackIndexBefore);
	TestTrue(TEXT("Force Restore rollback restores worktree bytes"), FFileHelper::LoadFileToArray(RollbackBytesAfter, *Filename));
	TestTrue(TEXT("Force Restore rollback keeps worktree bytes"), RollbackBytesAfter == RollbackBytesBefore);
	if (!Fixture.RunGit(TEXT("reset --hard HEAD")) || !Fixture.RunGit(FString::Printf(TEXT("checkout %s"), *MainBranch))) return false;
	return TestCallbacks(*this, RollbackConfirmCalls, RollbackPrepareCalls, RollbackReloadCalls);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetRejectionAutomationTest, "Cthulhu.GitSourceControl.AssetOperations.Rejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetRejectionAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("opaque tracked bytes\n")) || !Fixture.WriteFile(TEXT("Content/Plain.txt"), TEXT("plain tracked bytes\n")) || !Fixture.CommitAll(TEXT("Asset-operation rejection fixture"))) return false;
	FString CommitId;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), CommitId)) return false;
	CommitId.TrimStartAndEndInline();
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	int32 ConfirmCalls = 0;
	int32 PrepareCalls = 0;
	int32 ReloadCalls = 0;
	const FGitAssetOperationCallbacks Callbacks = MakeAcceptingCallbacks(ConfirmCalls, PrepareCalls, ReloadCalls);
	const FString AssetFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	FGitAssetOperationResult Result;
	TestFalse(TEXT("Cross-rename historical Restore is rejected"), Operations.RestoreRevisionToWorkspace(AssetFilename, CommitId, TEXT("Content/OldName.uasset"), Callbacks, Result));
	Result = FGitAssetOperationResult();
	const FString CaseOnlyAssetFilename = Fixture.AbsoluteFilename(TEXT("Content/tracked.uasset"));
	TestFalse(TEXT("Case-only path mismatch is treated as a cross-rename"), Operations.RestoreRevisionToWorkspace(CaseOnlyAssetFilename, CommitId, TEXT("Content/Tracked.uasset"), Callbacks, Result));
	Result = FGitAssetOperationResult();
	TestFalse(TEXT("Discard rejects non-uasset selections"), Operations.DiscardTrackedFiles({ Fixture.AbsoluteFilename(TEXT("Content/Plain.txt")) }, Callbacks, Result));
	TestEqual(TEXT("Rejected operations never prompt"), ConfirmCalls, 0);
	TestEqual(TEXT("Rejected operations never prepare mutation"), PrepareCalls, 0);
	return TestEqual(TEXT("Rejected operations never reload packages"), ReloadCalls, 0);
}

#endif
