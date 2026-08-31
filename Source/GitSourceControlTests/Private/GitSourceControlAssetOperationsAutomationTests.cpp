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
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)
				&& FFileHelper::SaveStringToFile(Contents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool ReadFile(const FString& RelativeFilename, FString& OutContents) const
		{
			return FFileHelper::LoadFileToString(OutContents, *AbsoluteFilename(RelativeFilename));
		}

		bool RunGit(const FString& Arguments, FString& OutOutput) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *Arguments);
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutOutput, &StandardError);
			if (ReturnCode == 0)
			{
				return true;
			}
			Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
			return false;
		}

		bool RunGit(const FString& Arguments) const
		{
			FString Output;
			return RunGit(Arguments, Output);
		}

		bool CommitAll(const FString& Message) const
		{
			return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(Message)));
		}

		FString AbsoluteFilename(const FString& RelativeFilename) const
		{
			return FPaths::Combine(Root, RelativeFilename);
		}

		const FString& GetGitBinary() const { return GitBinary; }
		const FString& GetRoot() const { return Root; }

	private:
		bool IsSafePath() const
		{
			const FString Parent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitSourceControlAssetOperationTests"));
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

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakeAcceptingCallbacks()
	{
		using namespace GitSourceControlAssetOperations;
		FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [](const FString&, const TArray<FString>&) { return true; };
		Callbacks.PrepareForMutation = [](const TArray<FString>&) { return true; };
		Callbacks.ReloadPackages = [](const TArray<FString>&) { return true; };
		return Callbacks;
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetDiscardAutomationTest, "UEGitPlugin.AssetOperations.Discard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetDiscardAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("head bytes\n"))
		|| !Fixture.CommitAll(TEXT("Initial tracked uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("staged bytes\n"))
		|| !Fixture.RunGit(TEXT("add -- Content/Tracked.uasset"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("unstaged bytes\n")))
	{
		return false;
	}

	const FString Filename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	FString IndexBeforeRollback;
	FString WorktreeBeforeRollback;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexBeforeRollback)
		|| !Fixture.ReadFile(TEXT("Content/Tracked.uasset"), WorktreeBeforeRollback))
	{
		return false;
	}

	FGitAssetOperationCallbacks RollbackCallbacks = MakeAcceptingCallbacks();
	RollbackCallbacks.AllowGitMutationForTesting = [](const FString& GitSubcommand)
	{
		return !GitSubcommand.Equals(TEXT("restore"), ESearchCase::CaseSensitive);
	};
	FGitAssetOperationResult RollbackResult;
	TestFalse(TEXT("Discard rolls back when Git restore cannot mutate"), Operations.DiscardTrackedFiles({ Filename }, RollbackCallbacks, RollbackResult));
	FString IndexAfterRollback;
	FString WorktreeAfterRollback;
	if (!TestTrue(TEXT("Discard rollback preserves the index"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterRollback))
		|| !TestTrue(TEXT("Discard rollback preserves the worktree"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), WorktreeAfterRollback)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Discard rollback restores the exact index snapshot"), IndexAfterRollback, IndexBeforeRollback)
		|| !TestEqual(TEXT("Discard rollback restores the exact worktree bytes"), WorktreeAfterRollback, WorktreeBeforeRollback))
	{
		return false;
	}

	FGitAssetOperationResult Result;
	if (!TestTrue(TEXT("Discard restores a tracked standalone asset"), Operations.DiscardTrackedFiles({ Filename }, MakeAcceptingCallbacks(), Result)))
	{
		AddError(FString::Join(Result.Errors, TEXT("\n")));
		return false;
	}
	FString Contents;
	FString Status;
	if (!TestTrue(TEXT("Discarded asset is readable"), Fixture.ReadFile(TEXT("Content/Tracked.uasset"), Contents))
		|| !TestTrue(TEXT("Discard status is readable"), Fixture.RunGit(TEXT("status --porcelain=v2 -- Content/Tracked.uasset"), Status)))
	{
		return false;
	}
	Status.TrimStartAndEndInline();
	return TestEqual(TEXT("Discard restores HEAD worktree bytes"), Contents, FString(TEXT("head bytes\n")))
		&& TestTrue(TEXT("Discard leaves no selected asset residue"), Status.IsEmpty())
		&& TestTrue(TEXT("Discard reports complete success"), Result.bSucceeded && Result.bReloadSucceeded);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetHistoryRestoreAutomationTest, "UEGitPlugin.AssetOperations.HistoryRestore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetHistoryRestoreAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}

	FScopedDisableValidateOnSave DisableValidateOnSave;
	const FString ContentDirectory = FPaths::Combine(Fixture.GetRoot(), TEXT("Content"));
	const FString MountRoot = FString::Printf(TEXT("/GitSourceControlAssetRestore_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory); };
	const FString RelativeFilename = TEXT("Content/Tracked.uasset");
	const FString Filename = Fixture.AbsoluteFilename(RelativeFilename);
	UPackage* Package = CreatePackage(*(MountRoot + TEXT("Tracked")));
	UCurveFloat* Asset = Package ? NewObject<UCurveFloat>(Package, TEXT("Tracked"), RF_Public | RF_Standalone) : nullptr;
	if (!TestNotNull(TEXT("Restore fixture package exists"), Package) || !TestNotNull(TEXT("Restore fixture asset exists"), Asset))
	{
		return false;
	}
	Asset->FloatCurve.AddKey(0.0f, 1.0f);
	if (!SaveCurvePackage(*this, Package, Asset, Filename, TEXT("first")) || !Fixture.CommitAll(TEXT("Initial restorable uasset")))
	{
		return false;
	}
	FString FirstCommit;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), FirstCommit))
	{
		return false;
	}
	FirstCommit.TrimStartAndEndInline();
	TArray<uint8> FirstBytes;
	if (!TestTrue(TEXT("First valid package bytes are available"), FFileHelper::LoadFileToArray(FirstBytes, *Filename)))
	{
		return false;
	}
	Asset->FloatCurve.AddKey(1.0f, 2.0f);
	if (!SaveCurvePackage(*this, Package, Asset, Filename, TEXT("second")) || !Fixture.CommitAll(TEXT("Current uasset revision")))
	{
		return false;
	}
	Package->SetDirtyFlag(false);
	TArray<UPackage*> PackagesToUnload;
	PackagesToUnload.Add(Package);
	FText UnloadError;
	if (!TestTrue(TEXT("Current package unloads before direct Restore"), UPackageTools::UnloadPackages(PackagesToUnload, UnloadError)))
	{
		AddError(UnloadError.ToString());
		return false;
	}

	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	FString IndexAtHead;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAtHead))
	{
		return false;
	}
	FGitAssetOperationCallbacks PartialReloadCallbacks = MakeAcceptingCallbacks();
	PartialReloadCallbacks.ReloadPackages = [](const TArray<FString>&) { return false; };
	FGitAssetOperationResult PartialReloadResult;
	TestFalse(TEXT("Restore reports a partial failure when reload fails"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, PartialReloadCallbacks, PartialReloadResult));
	TArray<uint8> RestoredBytes;
	FString IndexAfterRestore;
	if (!TestTrue(TEXT("Partial Restore writes readable package bytes"), FFileHelper::LoadFileToArray(RestoredBytes, *Filename))
		|| !TestTrue(TEXT("Partial Restore keeps the index readable"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterRestore)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Partial Restore writes the selected historical bytes"), RestoredBytes == FirstBytes)
		|| !TestEqual(TEXT("Partial Restore resets the index to HEAD"), IndexAfterRestore, IndexAtHead)
		|| !TestFalse(TEXT("Partial Restore reports reload failure"), PartialReloadResult.bSucceeded || PartialReloadResult.bReloadSucceeded))
	{
		return false;
	}

	FGitAssetOperationCallbacks RollbackCallbacks = MakeAcceptingCallbacks();
	RollbackCallbacks.AllowWorktreeReplaceForTesting = []() { return false; };
	FGitAssetOperationResult RollbackResult;
	TestFalse(TEXT("Restore rolls back when worktree replacement fails"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, RollbackCallbacks, RollbackResult));
	TArray<uint8> BytesAfterRollback;
	FString IndexAfterRollback;
	if (!TestTrue(TEXT("Restore rollback preserves package bytes"), FFileHelper::LoadFileToArray(BytesAfterRollback, *Filename))
		|| !TestTrue(TEXT("Restore rollback preserves the index"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Tracked.uasset"), IndexAfterRollback)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Restore rollback restores the worktree snapshot"), BytesAfterRollback == RestoredBytes)
		|| !TestEqual(TEXT("Restore rollback restores the index snapshot"), IndexAfterRollback, IndexAfterRestore))
	{
		return false;
	}

	FGitAssetOperationResult SuccessResult;
	return TestTrue(TEXT("Restore completes after the reload failure is cleared"), Operations.RestoreRevisionToWorkspace(Filename, FirstCommit, RelativeFilename, MakeAcceptingCallbacks(), SuccessResult))
		&& TestTrue(TEXT("Restore reports complete success"), SuccessResult.bSucceeded && SuccessResult.bReloadSucceeded);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlAssetRejectionAutomationTest, "UEGitPlugin.AssetOperations.Rejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAssetRejectionAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("opaque tracked bytes\n"))
		|| !Fixture.WriteFile(TEXT("Content/Plain.txt"), TEXT("plain tracked bytes\n"))
		|| !Fixture.CommitAll(TEXT("Asset-operation rejection fixture")))
	{
		return false;
	}
	FString CommitId;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), CommitId))
	{
		return false;
	}
	CommitId.TrimStartAndEndInline();
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetRoot());
	const FGitAssetOperationCallbacks Callbacks = MakeAcceptingCallbacks();
	const FString AssetFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	FGitAssetOperationResult Result;
	TestFalse(TEXT("Cross-rename historical Restore is rejected"), Operations.RestoreRevisionToWorkspace(AssetFilename, CommitId, TEXT("Content/OldName.uasset"), Callbacks, Result));
	Result = FGitAssetOperationResult();
	TestFalse(TEXT("Case-only historical path mismatch is rejected"), Operations.RestoreRevisionToWorkspace(Fixture.AbsoluteFilename(TEXT("Content/tracked.uasset")), CommitId, TEXT("Content/Tracked.uasset"), Callbacks, Result));
	Result = FGitAssetOperationResult();
	return TestFalse(TEXT("Discard rejects non-package selections"), Operations.DiscardTrackedFiles({ Fixture.AbsoluteFilename(TEXT("Content/Plain.txt")) }, Callbacks, Result));
}

#endif
