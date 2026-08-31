// Copyright (c) 2026

#include "GitMapPackageSet.h"
#include "GitSourceControlRevision.h"
#include "GitSourceControlUtils.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlLocalAutomationTestsPrivate
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

	bool RunGitAt(FAutomationTestBase& Test, const FString& GitBinary, const FString& Directory, const FString& Arguments, FString& OutOutput)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardError;
		const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Directory), *Arguments);
		FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutOutput, &StandardError);
		if (ReturnCode == 0)
		{
			return true;
		}
		Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): git %s\n%s"), ReturnCode, *CommandLine, *StandardError));
		return false;
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

	class FGitTestFixture final
	{
	public:
		explicit FGitTestFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FGitTestFixture()
		{
			if (!Directory.IsEmpty() && IsSafePath() && IFileManager::Get().DirectoryExists(*Directory))
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, true);
			}
		}

		bool Initialize()
		{
			GitBinary = GitSourceControlUtils::FindGitBinaryPath();
			if (GitBinary.IsEmpty())
			{
				Test.AddError(TEXT("Git executable is required for GitSourceControl automation tests."));
				return false;
			}
			Directory = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitSourceControlTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (!IsSafePath() || !IFileManager::Get().MakeDirectory(*Directory, true))
			{
				Test.AddError(FString::Printf(TEXT("Could not create safe fixture directory: %s"), *Directory));
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

		bool RunGit(const FString& Arguments, FString& OutOutput) const
		{
			return RunGitAt(Test, GitBinary, Directory, Arguments, OutOutput);
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
			return FPaths::Combine(Directory, RelativeFilename);
		}

		const FString& GetDirectory() const { return Directory; }
		const FString& GetGitBinary() const { return GitBinary; }

	private:
		bool IsSafePath() const
		{
			const FString FixtureParent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitSourceControlTests"));
			if (!IsSameOrUnderDirectory(Directory, FixtureParent) || IsSameOrUnderDirectory(Directory, FPaths::ProjectDir()))
			{
				return false;
			}
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (IsSameOrUnderDirectory(Directory, Plugin->GetBaseDir()))
				{
					return false;
				}
			}
			return true;
		}

		FAutomationTestBase& Test;
		FString GitBinary;
		FString Directory;
	};

	bool LoadHistory(FAutomationTestBase& Test, const FGitTestFixture& Fixture, const FString& Filename, EGitLocalSourceControlHistoryMode Mode,
		FString& OutHead, bool& bOutHeadChanged, TArray<FGitStandaloneHistoryTestEntry>& OutHistory)
	{
		FString Error;
		if (GitSourceControlUtils::Testing::LoadStandaloneHistory(Fixture.GetGitBinary(), Fixture.GetDirectory(), Filename, Mode, OutHead, bOutHeadChanged, OutHistory, Error))
		{
			return true;
		}
		Test.AddError(Error);
		return false;
	}

	bool IsGitLfsAvailable(const FString& GitBinary)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*GitBinary, TEXT("lfs version"), &ReturnCode, &StandardOutput, &StandardError);
		return ReturnCode == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlHistoryAndBlobAutomationTest, "UEGitPlugin.Local.HistoryAndBlob", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlHistoryAndBlobAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize()
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("tracked first revision\n"))
		|| !Fixture.WriteFile(TEXT("Content/Rename Old.uasset"), TEXT("old package header\n"))
		|| !Fixture.WriteFile(TEXT("Content/Rename Old.uexp"), TEXT("old package sidecar\n"))
		|| !Fixture.CommitAll(TEXT("Initial history fixture"))
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("tracked second revision\n"))
		|| !Fixture.CommitAll(TEXT("Second history fixture revision")))
	{
		return false;
	}

	const FString TrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.uasset"));
	FString CapturedHead;
	bool bHeadChanged = false;
	TArray<FGitStandaloneHistoryTestEntry> History;
	if (!TestTrue(TEXT("Current-path history query succeeds"), LoadHistory(*this, Fixture, TrackedFilename, EGitLocalSourceControlHistoryMode::CurrentPath, CapturedHead, bHeadChanged, History))
		|| !TestEqual(TEXT("Current-path history has two revisions"), History.Num(), 2))
	{
		return false;
	}
	if (!TestFalse(TEXT("Stable history query keeps HEAD fixed"), bHeadChanged)
		|| !TestEqual(TEXT("Current-path history is pinned to captured HEAD"), History[0].CommitId, CapturedHead)
		|| !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("tracked third revision\n"))
		|| !Fixture.CommitAll(TEXT("Advance HEAD after history snapshot")))
	{
		return false;
	}
	if (!TestEqual(TEXT("Returned history remains the fixed HEAD snapshot"), History.Num(), 2)
		|| !TestEqual(TEXT("Returned newest revision remains the fixed HEAD"), History[0].CommitId, CapturedHead))
	{
		return false;
	}

	const FString RenameOld = TEXT("Content/Rename Old.uasset");
	const FString RenameMid = TEXT("Content/Rename Mid.uasset");
	const FString RenameNew = TEXT("Content/Rename New.uasset");
	if (!Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(RenameOld), *QuoteGitArgument(RenameMid)))
		|| !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(TEXT("Content/Rename Old.uexp")), *QuoteGitArgument(TEXT("Content/Rename Mid.uexp"))))
		|| !Fixture.CommitAll(TEXT("First exact package rename"))
		|| !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(RenameMid), *QuoteGitArgument(RenameNew)))
		|| !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(TEXT("Content/Rename Mid.uexp")), *QuoteGitArgument(TEXT("Content/Rename New.uexp"))))
		|| !Fixture.CommitAll(TEXT("Second exact package rename")))
	{
		return false;
	}

	FString ExactRenameHead;
	TArray<FGitStandaloneHistoryTestEntry> ExactRenameHistory;
	const FString RenameNewFilename = Fixture.AbsoluteFilename(RenameNew);
	if (!TestTrue(TEXT("Exact-rename history query succeeds"), LoadHistory(*this, Fixture, RenameNewFilename, EGitLocalSourceControlHistoryMode::ExactRenames, ExactRenameHead, bHeadChanged, ExactRenameHistory))
		|| !TestEqual(TEXT("Exact-rename history returns the R100 chain"), ExactRenameHistory.Num(), 3))
	{
		return false;
	}
	if (!TestEqual(TEXT("Exact-rename history is pinned to its captured HEAD"), ExactRenameHistory[0].CommitId, ExactRenameHead)
		|| !TestEqual(TEXT("Exact-rename newest path"), ExactRenameHistory[0].HistoricalPath, RenameNew)
		|| !TestEqual(TEXT("Exact-rename middle path"), ExactRenameHistory[1].HistoricalPath, RenameMid)
		|| !TestEqual(TEXT("Exact-rename oldest path"), ExactRenameHistory[2].HistoricalPath, RenameOld))
	{
		return false;
	}

	FGitChangedPrimaryPackageTarget HistoricalTarget;
	HistoricalTarget.PackageName = TEXT("/Game/HistoryRename");
	HistoricalTarget.RepositoryRelativePath = ExactRenameHistory[2].HistoricalPath;
	HistoricalTarget.AbsoluteFilename = RenameNewFilename;
	FGitPackageRevisionArtifactSet ArtifactSet;
	FString Error;
	if (!TestTrue(TEXT("Historical old-stem artifact set is complete"), GitMapPackageSet::BuildPackageRevisionArtifactSet(
		Fixture.GetGitBinary(), Fixture.GetDirectory(), ExactRenameHistory[2].CommitId, HistoricalTarget, ArtifactSet, Error)))
	{
		AddError(Error);
		return false;
	}
	FGitPackageRevisionMaterialization Materialization;
	if (!TestTrue(TEXT("Historical old-stem package materializes with sidecars"), GitMapPackageSet::MaterializePackageRevisionArtifacts(
		Fixture.GetGitBinary(), Fixture.GetDirectory(), ArtifactSet, Materialization, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString MaterializationDirectory = Materialization.TemporaryDirectory;
	GitSourceControlRevision::RegisterTemporaryExportDirectory(MaterializationDirectory);
	bool bMaterializationReleased = false;
	ON_SCOPE_EXIT
	{
		if (!bMaterializationReleased)
		{
			GitSourceControlRevision::ReleaseTemporaryExportDirectory(MaterializationDirectory);
		}
	};
	const FString* SidecarFilename = Materialization.FilenameByRepositoryRelativePath.Find(TEXT("Content/Rename Old.uexp"));
	if (!TestTrue(TEXT("Materialization preserves the historical package stem"), FPaths::GetCleanFilename(Materialization.PrimaryFilename).Equals(TEXT("Rename Old.uasset"), ESearchCase::CaseSensitive))
		|| !TestTrue(TEXT("Materialization includes the historical sidecar"), SidecarFilename != nullptr && FPaths::FileExists(*SidecarFilename)))
	{
		return false;
	}
	GitSourceControlRevision::ReleaseTemporaryExportDirectory(MaterializationDirectory);
	bMaterializationReleased = true;
	return TestFalse(TEXT("Historical materialization directory is released by its owner"), IFileManager::Get().DirectoryExists(*MaterializationDirectory));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlStartupCapabilityGateAutomationTest, "UEGitPlugin.Standalone.StartupCapabilityGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlStartupCapabilityGateAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const GitSourceControlUtils::FGitStartupCapability SavedCapability = GitSourceControlUtils::GetStartupGitCapability();
	ON_SCOPE_EXIT { GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(SavedCapability); };

	GitSourceControlUtils::FGitStartupCapability PendingCapability;
	PendingCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Pending;
	PendingCapability.Diagnostic = TEXT("Checking for Git 2.53.0 or newer...");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(PendingCapability);
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FString GitBinary;
	FString RepositoryRoot;
	FString Error;
	TestFalse(TEXT("Pending startup capability blocks repository resolution"), GitSourceControlUtils::ResolveStandaloneRepositoryForFile(FPaths::GetProjectFilePath(), GitBinary, RepositoryRoot, Error));
	if (!TestEqual(TEXT("Pending capability starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0))
		|| !TestTrue(TEXT("Pending capability reports a useful diagnostic"), Error.Contains(TEXT("Git 2.53.0"))))
	{
		return false;
	}

	GitSourceControlUtils::FGitStartupCapability UnavailableCapability;
	UnavailableCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Unavailable;
	UnavailableCapability.Diagnostic = TEXT("Detected Git executable: C:/Tools/Git/bin/git.exe\nDetected version: git version 2.42.0\nGit 2.53.0 or a newer release is required. Install or upgrade Git, then restart the Editor.");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(UnavailableCapability);
	Error.Reset();
	TestFalse(TEXT("Unavailable startup capability blocks repository resolution"), GitSourceControlUtils::ResolveStandaloneRepositoryForFile(FPaths::GetProjectFilePath(), GitBinary, RepositoryRoot, Error));
	return TestEqual(TEXT("Unavailable capability starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0))
		&& TestTrue(TEXT("Unavailable capability preserves repair guidance"), Error.Contains(TEXT("C:/Tools/Git/bin/git.exe")) && Error.Contains(TEXT("restart the Editor")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsRevisionFetchAutomationTest, "UEGitPlugin.Local.LfsRevisionFetch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsRevisionFetchAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	if (!IsGitLfsAvailable(Fixture.GetGitBinary()))
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping the local bare LFS fetch fixture."));
		return true;
	}

	const FString Origin = Fixture.AbsoluteFilename(TEXT("origin.git"));
	const FString Source = Fixture.AbsoluteFilename(TEXT("source"));
	const FString Clone = Fixture.AbsoluteFilename(TEXT("clone"));
	FString Output;
	if (!RunGitAt(*this, Fixture.GetGitBinary(), Fixture.GetDirectory(), FString::Printf(TEXT("init --bare %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Fixture.GetDirectory(), FString::Printf(TEXT("init %s"), *QuoteGitArgument(Source)), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("config user.name \"GitSourceControlTests\""), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("config user.email \"git-source-control-tests@example.invalid\""), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("lfs install --local"), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("lfs track \"*.bin\""), Output))
	{
		return false;
	}
	const FString HistoricalPayloadPath = TEXT("Content/Lfs Payload.bin");
	const FString Payload = FPaths::Combine(Source, HistoricalPayloadPath);
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Payload), true)
		|| !FFileHelper::SaveStringToFile(TEXT("LFS revision payload\n"), *Payload, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, FString::Printf(TEXT("add .gitattributes %s"), *QuoteGitArgument(HistoricalPayloadPath)), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("commit --no-gpg-sign -m \"LFS fetch fixture\""), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("push -u origin HEAD"), Output))
	{
		return false;
	}
	FString Branch;
	if (!RunGitAt(*this, Fixture.GetGitBinary(), Source, TEXT("branch --show-current"), Branch))
	{
		return false;
	}
	Branch.TrimStartAndEndInline();
	if (Branch.IsEmpty()
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Origin, FString::Printf(TEXT("symbolic-ref HEAD %s"), *QuoteGitArgument(FString::Printf(TEXT("refs/heads/%s"), *Branch))), Output)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Fixture.GetDirectory(), FString::Printf(TEXT("clone %s %s"), *QuoteGitArgument(Origin), *QuoteGitArgument(Clone)), Output))
	{
		return false;
	}

	FString CommitId;
	FString PointerText;
	if (!RunGitAt(*this, Fixture.GetGitBinary(), Clone, TEXT("rev-parse HEAD"), CommitId)
		|| !RunGitAt(*this, Fixture.GetGitBinary(), Clone, FString::Printf(TEXT("show %s"), *QuoteGitArgument(TEXT("HEAD:Content/Lfs Payload.bin"))), PointerText))
	{
		return false;
	}
	CommitId.TrimStartAndEndInline();
	FString Oid;
	int64 Size = -1;
	if (!TestTrue(TEXT("LFS pointer metadata parses"), ParseLfsPointer(PointerText, Oid, Size)))
	{
		return false;
	}
	const FString ObjectFilename = FPaths::Combine(Clone, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
	if (!TestTrue(TEXT("Clone receives the LFS object"), IFileManager::Get().FileExists(*ObjectFilename))
		|| !TestTrue(TEXT("Remove the local LFS object before targeted fetch"), IFileManager::Get().Delete(*ObjectFilename, false, true)))
	{
		return false;
	}

	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FString FetchError;
	if (!TestTrue(TEXT("Explicit history request fetches one exact LFS revision path"), GitSourceControlUtils::FetchLfsContentForRevision(
		Fixture.GetGitBinary(), Clone, CommitId, HistoricalPayloadPath, FetchError)))
	{
		AddError(FetchError);
		return false;
	}
	if (!TestEqual(TEXT("Explicit LFS request starts one targeted fetch"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(1))
		|| !TestTrue(TEXT("Fetched object matches pointer hash and size"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Clone, ObjectFilename, Oid, Size, FetchError)))
	{
		AddError(FetchError);
		return false;
	}
	const FString WrongOid = FString::ChrN(64, TEXT('0'));
	TestFalse(TEXT("LFS object rejects an incorrect expected hash"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Clone, ObjectFilename, WrongOid, Size, FetchError));
	TestFalse(TEXT("LFS object rejects an incorrect expected size"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Clone, ObjectFilename, Oid, Size + 1, FetchError));

	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FetchError.Reset();
	TestFalse(TEXT("Unsafe LFS literal path is rejected"), GitSourceControlUtils::FetchLfsContentForRevision(Fixture.GetGitBinary(), Clone, CommitId, TEXT("Content/*.bin"), FetchError));
	if (!TestEqual(TEXT("Unsafe literal path starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0)))
	{
		return false;
	}

	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	CancellationContext->Cancel();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
	FetchError.Reset();
	TestFalse(TEXT("Pre-cancelled LFS fetch is rejected"), GitSourceControlUtils::FetchLfsContentForRevision(Fixture.GetGitBinary(), Clone, CommitId, HistoricalPayloadPath, FetchError));
	return TestEqual(TEXT("Pre-cancelled LFS fetch starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0))
		&& TestTrue(TEXT("Pre-cancelled LFS fetch reports cancellation"), FetchError.Contains(TEXT("cancel"), ESearchCase::IgnoreCase));
}

#endif

IMPLEMENT_MODULE(FDefaultModuleImpl, GitSourceControlTests)
