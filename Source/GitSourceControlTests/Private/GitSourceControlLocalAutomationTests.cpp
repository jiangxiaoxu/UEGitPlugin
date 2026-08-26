// Copyright (c) 2026

#include "GitSourceControlAssetOperations.h"
#include "GitLocalSourceControl.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"

#include "Async/Async.h"
#include "AssetToolsModule.h"
#include "Curves/CurveFloat.h"
#include "DiffUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "ISourceControlProvider.h"
#include "Factories/CurveFactory.h"
#include "IAssetTools.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlLocalAutomationTestsPrivate
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
			return RunGit(TEXT("init")) && RunGit(TEXT("config user.name \"GitSourceControlTests\"")) && RunGit(TEXT("config user.email \"git-source-control-tests@example.invalid\""))
				&& RunGit(TEXT("config color.ui always"));
		}

		bool WriteFile(const FString& RelativeFilename, const FString& Contents) const
		{
			const FString Filename = AbsoluteFilename(RelativeFilename);
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true) && FFileHelper::SaveStringToFile(Contents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool DeleteFile(const FString& RelativeFilename) const
		{
			return FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*AbsoluteFilename(RelativeFilename));
		}

		bool RunGit(const FString& Arguments, FString& OutOutput) const { return RunGitAt(Test, GitBinary, Directory, Arguments, OutOutput); }
		bool RunGit(const FString& Arguments) const { FString Output; return RunGit(Arguments, Output); }
		bool CommitAll(const FString& Message) const { return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(Message))); }
		FString AbsoluteFilename(const FString& RelativeFilename) const { return FPaths::Combine(Directory, RelativeFilename); }
		FString RelativeFilename(const FString& AbsoluteFilename) const
		{
			FString Result = FPaths::ConvertRelativePathToFull(AbsoluteFilename);
			FPaths::NormalizeFilename(Result);
			FString Prefix = Directory + TEXT("/");
			if (!FPaths::MakePathRelativeTo(Result, *Prefix))
			{
				Test.AddError(FString::Printf(TEXT("Fixture path is outside the repository: %s"), *AbsoluteFilename));
				return FString();
			}
			FPaths::NormalizeFilename(Result);
			return Result;
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

	bool CreateCommittedFixture(FGitTestFixture& Fixture)
	{
		return Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("initial\n"))
			&& Fixture.WriteFile(TEXT("Content/Rename Old.txt"), TEXT("rename\n"))
			&& Fixture.CommitAll(TEXT("Initial fixture"));
	}

	bool IsGitLfsAvailable(const FString& GitBinary)
	{
		int32 ReturnCode = INDEX_NONE;
		FString StandardOutput;
		FString StandardError;
		FPlatformProcess::ExecProcess(*GitBinary, TEXT("lfs version"), &ReturnCode, &StandardOutput, &StandardError);
		return ReturnCode == 0;
	}

	bool LoadHistory(FAutomationTestBase& Test, const FGitTestFixture& Fixture, const FString& Filename, EGitLocalSourceControlHistoryMode Mode, FString& OutHead, bool& bOutHeadChanged, TArray<FGitStandaloneHistoryTestEntry>& OutHistory)
	{
		FString Error;
		if (GitSourceControlUtils::Testing::LoadStandaloneHistory(Fixture.GetGitBinary(), Fixture.GetDirectory(), Filename, Mode, OutHead, bOutHeadChanged, OutHistory, Error))
		{
			return true;
		}
		Test.AddError(Error);
		return false;
	}

	bool WaitForOperation(FAutomationTestBase& Test, UGitLocalSourceControlOperation* Operation, const FString& Label)
	{
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s returns an operation"), *Label), Operation))
		{
			return false;
		}
		const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(15.0);
		while (!Operation->IsTerminal() && FDateTime::UtcNow() < Deadline)
		{
			Operation->Tick();
			FPlatformProcess::SleepNoStats(0.01f);
		}
		Operation->Tick();
		return Test.TestTrue(*FString::Printf(TEXT("%s reaches a terminal state"), *Label), Operation->IsTerminal());
	}

	bool SaveCurvePackage(FAutomationTestBase& Test, UPackage* Package, UCurveFloat* Asset, const FString& Filename, const FString& Revision)
	{
		FMetaData& Metadata = Package->GetMetaData();
		Metadata.SetValue(Asset, TEXT("GitSourceControlIntegrationRevision"), *Revision);
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		return Test.TestTrue(*FString::Printf(TEXT("Saved integration package revision %s"), *Revision), UPackage::SavePackage(Package, Asset, *Filename, SaveArgs));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlHistoryAndBlobAutomationTest, "Cthulhu.GitSourceControl.Local.HistoryAndBlob", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlHistoryAndBlobAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture) || !Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("second revision\n")) || !Fixture.CommitAll(TEXT("Second fixture revision")))
	{
		return false;
	}
	const FString TrackedFilename = Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt"));
	FString CapturedHead;
	bool bHeadChanged = false;
	TArray<FGitStandaloneHistoryTestEntry> History;
	if (!TestTrue(TEXT("Current-path history query succeeds"), LoadHistory(*this, Fixture, TrackedFilename, EGitLocalSourceControlHistoryMode::CurrentPath, CapturedHead, bHeadChanged, History)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Current-path history has two revisions"), History.Num(), 2))
	{
		return false;
	}
	TestFalse(TEXT("Stable fixture leaves HEAD unchanged during the query"), bHeadChanged);
	TestEqual(TEXT("Current revision is the captured immutable HEAD"), History[0].CommitId, CapturedHead);

	const FString ExportFilename = Fixture.AbsoluteFilename(TEXT("Exported/Tracked.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExportFilename), true);
	FString ExportError;
	if (!TestTrue(TEXT("Captured HEAD blob export succeeds"), GitSourceControlUtils::DumpRevisionBlobToFile(Fixture.GetGitBinary(), Fixture.GetDirectory(), CapturedHead + TEXT(":Content/Tracked.txt"), ExportFilename, ExportError)))
	{
		AddError(ExportError);
		return false;
	}
	FString ExportedContents;
	TestTrue(TEXT("Exported blob can be read"), FFileHelper::LoadFileToString(ExportedContents, *ExportFilename));
	TestEqual(TEXT("Exported blob contains captured content"), ExportedContents, FString(TEXT("second revision\n")));

	if (!Fixture.WriteFile(TEXT("Content/Tracked.txt"), TEXT("third revision after snapshot\n")) || !Fixture.CommitAll(TEXT("Advance HEAD after history snapshot")))
	{
		return false;
	}
	TestEqual(TEXT("Returned history remains the captured snapshot after HEAD advances"), History.Num(), 2);
	TestEqual(TEXT("Returned newest revision remains captured after HEAD advances"), History[0].CommitId, CapturedHead);
	FString RefreshedHead;
	TArray<FGitStandaloneHistoryTestEntry> RefreshedHistory;
	if (!TestTrue(TEXT("A later history query refreshes the snapshot"), LoadHistory(*this, Fixture, TrackedFilename, EGitLocalSourceControlHistoryMode::CurrentPath, RefreshedHead, bHeadChanged, RefreshedHistory)))
	{
		return false;
	}
	TestFalse(TEXT("New history captures the new HEAD"), RefreshedHead.Equals(CapturedHead, ESearchCase::CaseSensitive));
	TestEqual(TEXT("Refreshed history has three revisions"), RefreshedHistory.Num(), 3);

	const FString RenameOld = Fixture.AbsoluteFilename(TEXT("Content/Rename Old.txt"));
	const FString RenameMid = Fixture.AbsoluteFilename(TEXT("Content/Rename Mid.txt"));
	const FString RenameNew = Fixture.AbsoluteFilename(TEXT("Content/Rename New.txt"));
	if (!Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(Fixture.RelativeFilename(RenameOld)), *QuoteGitArgument(Fixture.RelativeFilename(RenameMid)))) || !Fixture.CommitAll(TEXT("First exact rename"))
		|| !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(Fixture.RelativeFilename(RenameMid)), *QuoteGitArgument(Fixture.RelativeFilename(RenameNew)))) || !Fixture.CommitAll(TEXT("Second exact rename")))
	{
		return false;
	}
	FString CurrentRenameHead;
	TArray<FGitStandaloneHistoryTestEntry> CurrentPathRenameHistory;
	if (!TestTrue(TEXT("Current-path rename history query succeeds"), LoadHistory(*this, Fixture, RenameNew, EGitLocalSourceControlHistoryMode::CurrentPath, CurrentRenameHead, bHeadChanged, CurrentPathRenameHistory)))
	{
		return false;
	}
	TestFalse(TEXT("Current-path history never falls back to old paths"), CurrentPathRenameHistory.ContainsByPredicate([](const FGitStandaloneHistoryTestEntry& Revision)
	{
		return Revision.HistoricalPath.Equals(TEXT("Content/Rename Old.txt"), ESearchCase::CaseSensitive) || Revision.HistoricalPath.Equals(TEXT("Content/Rename Mid.txt"), ESearchCase::CaseSensitive);
	}));
	FString ExactRenameHead;
	TArray<FGitStandaloneHistoryTestEntry> ExactRenameHistory;
	if (!TestTrue(TEXT("Exact-rename history query succeeds"), LoadHistory(*this, Fixture, RenameNew, EGitLocalSourceControlHistoryMode::ExactRenames, ExactRenameHead, bHeadChanged, ExactRenameHistory)))
	{
		return false;
	}
	if (!TestEqual(TEXT("Exact-rename history returns the committed R100 chain"), ExactRenameHistory.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("Exact-rename newest path"), ExactRenameHistory[0].HistoricalPath, FString(TEXT("Content/Rename New.txt")));
	TestEqual(TEXT("Exact-rename middle path"), ExactRenameHistory[1].HistoricalPath, FString(TEXT("Content/Rename Mid.txt")));
	TestEqual(TEXT("Exact-rename oldest path"), ExactRenameHistory[2].HistoricalPath, FString(TEXT("Content/Rename Old.txt")));
	TestEqual(TEXT("Historical revision retains current workspace filename"), ExactRenameHistory[2].LocalFilename, RenameNew);
	const FString OldRenameExport = Fixture.AbsoluteFilename(TEXT("Exported/Rename Old.txt"));
	FString OldRenameExportError;
	if (!TestTrue(TEXT("Oldest exact-rename blob export succeeds"), GitSourceControlUtils::DumpRevisionBlobToFile(Fixture.GetGitBinary(), Fixture.GetDirectory(), FString::Printf(TEXT("%s:%s"), *ExactRenameHistory[2].CommitId, *ExactRenameHistory[2].HistoricalPath), OldRenameExport, OldRenameExportError)))
	{
		AddError(OldRenameExportError);
		return false;
	}

	const FString NonExactOld = Fixture.AbsoluteFilename(TEXT("Content/Non Exact Old.txt"));
	const FString NonExactNew = Fixture.AbsoluteFilename(TEXT("Content/Non Exact New.txt"));
	if (!Fixture.WriteFile(TEXT("Content/Non Exact Old.txt"), TEXT("before\n")) || !Fixture.CommitAll(TEXT("Non-exact source"))
		|| !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(Fixture.RelativeFilename(NonExactOld)), *QuoteGitArgument(Fixture.RelativeFilename(NonExactNew))))
		|| !Fixture.WriteFile(TEXT("Content/Non Exact New.txt"), TEXT("replacement payload\n")) || !Fixture.CommitAll(TEXT("Non-exact move")))
	{
		return false;
	}
	FString NonExactHead;
	TArray<FGitStandaloneHistoryTestEntry> NonExactHistory;
	if (!TestTrue(TEXT("Non-exact rename query succeeds"), LoadHistory(*this, Fixture, NonExactNew, EGitLocalSourceControlHistoryMode::ExactRenames, NonExactHead, bHeadChanged, NonExactHistory)))
	{
		return false;
	}
	TestEqual(TEXT("Non-R100 rename does not infer an old path"), NonExactHistory.Num(), 1);

	const FString ReaddedFilename = Fixture.AbsoluteFilename(TEXT("Content/Delete Readd.txt"));
	if (!Fixture.WriteFile(TEXT("Content/Delete Readd.txt"), TEXT("original\n")) || !Fixture.CommitAll(TEXT("Delete-readd original")) || !Fixture.DeleteFile(TEXT("Content/Delete Readd.txt")) || !Fixture.CommitAll(TEXT("Delete-readd delete"))
		|| !Fixture.WriteFile(TEXT("Content/Delete Readd.txt"), TEXT("replacement\n")) || !Fixture.CommitAll(TEXT("Delete-readd replacement")))
	{
		return false;
	}
	FString ReaddedHead;
	TArray<FGitStandaloneHistoryTestEntry> ReaddedHistory;
	return TestTrue(TEXT("Delete-readd query succeeds"), LoadHistory(*this, Fixture, ReaddedFilename, EGitLocalSourceControlHistoryMode::ExactRenames, ReaddedHead, bHeadChanged, ReaddedHistory))
		&& TestEqual(TEXT("Delete-readd stops at the replacement lineage birth"), ReaddedHistory.Num(), 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlIntegrationHistoryDiffRestoreAutomationTest, "Cthulhu.GitSourceControl.Integration.HistoryDiffRestore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlIntegrationHistoryDiffRestoreAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlAssetOperations;
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	if (!IsGitLfsAvailable(Fixture.GetGitBinary()))
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping HistoryDiffRestore integration fixture."));
		return true;
	}
	if (!Fixture.RunGit(TEXT("lfs install --local")) || !Fixture.RunGit(TEXT("lfs track \"Content/*.uasset\"")))
	{
		return false;
	}
	FScopedDisableValidateOnSave DisableValidateOnSave;
	const FString ContentDirectory = FPaths::Combine(Fixture.GetDirectory(), TEXT("Content"));
	const FString MountRoot = FString::Printf(TEXT("/GitSourceControlIntegration_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory); };
	const FString AssetName = TEXT("HistoryDiffFixture");
	const FString PackageName = MountRoot + AssetName;
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UPackage* Package = CreatePackage(*PackageName);
	UCurveFloat* Asset = Package ? NewObject<UCurveFloat>(Package, *AssetName, RF_Public | RF_Standalone) : nullptr;
	if (!TestNotNull(TEXT("Integration package exists"), Package) || !TestNotNull(TEXT("Integration curve exists"), Asset) || !Fixture.WriteFile(TEXT(".gitignore"), TEXT("Origin.git/\n")))
	{
		return false;
	}
	Asset->FloatCurve.AddKey(0.0f, 1.0f);
	if (!SaveCurvePackage(*this, Package, Asset, PackageFilename, TEXT("first")) || !Fixture.CommitAll(TEXT("Integration first package revision")))
	{
		return false;
	}
	const FString OriginDirectory = Fixture.AbsoluteFilename(TEXT("Origin.git"));
	if (!Fixture.RunGit(FString::Printf(TEXT("init --bare %s"), *QuoteGitArgument(OriginDirectory))) || !Fixture.RunGit(FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(OriginDirectory))) || !Fixture.RunGit(TEXT("push -u origin HEAD")))
	{
		return false;
	}
	FString FirstCommit;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), FirstCommit)) return false;
	FirstCommit.TrimStartAndEndInline();
	Asset->FloatCurve.AddKey(1.0f, 2.0f);
	if (!SaveCurvePackage(*this, Package, Asset, PackageFilename, TEXT("second")) || !Fixture.CommitAll(TEXT("Integration second package revision"))) return false;
	FString CapturedHead;
	bool bHeadChanged = false;
	TArray<FGitStandaloneHistoryTestEntry> History;
	if (!TestTrue(TEXT("Standalone package history loads"), LoadHistory(*this, Fixture, PackageFilename, EGitLocalSourceControlHistoryMode::CurrentPath, CapturedHead, bHeadChanged, History)) || !TestEqual(TEXT("Package history has two revisions"), History.Num(), 2)) return false;
	TestEqual(TEXT("Package history keeps the first commit"), History[1].CommitId, FirstCommit);
	FString PointerText;
	if (!Fixture.RunGit(FString::Printf(TEXT("show %s:%s"), *History[1].CommitId, *History[1].HistoricalPath), PointerText)) return false;
	FString LfsOid;
	int64 LfsSize = 0;
	if (!TestTrue(TEXT("Historical package contains an LFS pointer"), ParseLfsPointer(PointerText, LfsOid, LfsSize))) return false;
	const FString LfsObjectFilename = FPaths::Combine(Fixture.GetDirectory(), TEXT(".git"), TEXT("lfs"), TEXT("objects"), LfsOid.Left(2), LfsOid.Mid(2, 2), LfsOid);
	FString LfsError;
	if (!TestTrue(TEXT("Local LFS object verifies before public fetch"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetDirectory(), LfsObjectFilename, LfsOid, LfsSize, LfsError)))
	{
		AddError(LfsError);
		return false;
	}
	const FString AssetObjectPath = PackageName + TEXT(".") + AssetName;
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	UGitLocalSourceControlOperation* LocalHitFetch = UGitLocalSourceControlLibrary::StartFetchLfsRevision(AssetObjectPath, History[1].CommitId);
	if (!WaitForOperation(*this, LocalHitFetch, TEXT("StartFetchLfsRevision local cache hit"))) return false;
	TestTrue(TEXT("StartFetchLfsRevision local cache hit succeeds"), LocalHitFetch->GetResult().bSucceeded);
	TestEqual(TEXT("StartFetchLfsRevision local cache hit starts no LFS fetch"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(0));
	if (!TestTrue(TEXT("StartFetchLfsRevision local cache hit verifies the materialized object"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetDirectory(), LfsObjectFilename, LfsOid, LfsSize, LfsError)))
	{
		AddError(LfsError);
		return false;
	}
	FString FirstTempFilename;
	if (!TestTrue(TEXT("Historical LFS export uses the local cache hit"),
		GitSourceControlUtils::Testing::ExportStandaloneRevisionForDiff(Fixture.GetGitBinary(), Fixture.GetDirectory(), PackageFilename, History[1].CommitId, History[1].HistoricalPath, FirstTempFilename))) return false;
	if (!TestTrue(TEXT("Remove local LFS object before public cache miss"), IFileManager::Get().Delete(*LfsObjectFilename, false, true, true))) return false;
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	UGitLocalSourceControlOperation* LocalMissFetch = UGitLocalSourceControlLibrary::StartFetchLfsRevision(AssetObjectPath, History[1].CommitId);
	if (!WaitForOperation(*this, LocalMissFetch, TEXT("StartFetchLfsRevision local cache miss"))) return false;
	TestTrue(TEXT("StartFetchLfsRevision local cache miss succeeds"), LocalMissFetch->GetResult().bSucceeded);
	TestEqual(TEXT("StartFetchLfsRevision local cache miss performs one targeted LFS fetch"), GitSourceControlUtils::Testing::GetGitLfsFetchLaunchCount(), static_cast<uint64>(1));
	if (!TestTrue(TEXT("StartFetchLfsRevision local cache miss verifies the downloaded object"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetDirectory(), LfsObjectFilename, LfsOid, LfsSize, LfsError)))
	{
		AddError(LfsError);
		return false;
	}
	if (!TestTrue(TEXT("Remove local LFS object before revision adapter cache miss"), IFileManager::Get().Delete(*LfsObjectFilename, false, true, true))) return false;
	FString SecondTempFilename;
	if (!TestTrue(TEXT("Standalone revision adapter fetches LFS cache miss"),
		GitSourceControlUtils::Testing::ExportStandaloneRevisionForDiff(Fixture.GetGitBinary(), Fixture.GetDirectory(), PackageFilename, History[1].CommitId, History[1].HistoricalPath, SecondTempFilename)) || !TestTrue(TEXT("Targeted LFS fetch verifies"), GitSourceControlUtils::VerifyLocalLfsObject(Fixture.GetGitBinary(), Fixture.GetDirectory(), LfsObjectFilename, LfsOid, LfsSize, LfsError)))
	{
		AddError(LfsError);
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*FirstTempFilename, false, true, true); IFileManager::Get().Delete(*SecondTempFilename, false, true, true); };
	TestFalse(TEXT("Revision exports use unique temporary paths"), FirstTempFilename.Equals(SecondTempFilename, ESearchCase::CaseSensitive));
	UPackage* DiffPackage = GitSourceControlUtils::Testing::LoadStandaloneRevisionPackageForDiff(
		Fixture.GetGitBinary(), Fixture.GetDirectory(), PackageFilename, History[1].CommitId, History[1].HistoricalPath);
	if (!TestNotNull(TEXT("DiffUtils loads a package through the standalone revision adapter"), DiffPackage) || !TestNotNull(TEXT("Diff package contains an asset"), DiffPackage->FindAssetInPackage())) return false;
	TArray<uint8> ExpectedBytes;
	if (!TestTrue(TEXT("Historical package bytes are readable"), FFileHelper::LoadFileToArray(ExpectedBytes, *FirstTempFilename))) return false;
	Package->SetDirtyFlag(false);
	DiffPackage->SetDirtyFlag(false);
	TArray<UPackage*> PackagesToUnload;
	PackagesToUnload.Add(Package);
	PackagesToUnload.Add(DiffPackage);
	FText UnloadError;
	if (!TestTrue(TEXT("Packages unload before Restore"), UPackageTools::UnloadPackages(PackagesToUnload, UnloadError))) { AddError(UnloadError.ToString()); return false; }
	Package = nullptr;
	DiffPackage = nullptr;
	if (!TestTrue(TEXT("Remove LFS object before Restore fetch"), IFileManager::Get().Delete(*LfsObjectFilename, false, true, true))) return false;
	FString IndexBefore;
	if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/HistoryDiffFixture.uasset"), IndexBefore)) return false;
	FGitSourceControlAssetOperations Operations(Fixture.GetGitBinary(), Fixture.GetDirectory());
	FGitAssetOperationCallbacks Callbacks;
	Callbacks.Confirm = [](const FString&, const TArray<FString>&) { return true; };
	Callbacks.PrepareForMutation = [](const TArray<FString>&) { return true; };
	Callbacks.ReloadPackages = [](const TArray<FString>&) { return true; };
	FGitAssetOperationResult RestoreResult;
	if (!TestTrue(TEXT("Same-path historical LFS Restore succeeds"), Operations.RestoreRevisionToWorkspace(PackageFilename, History[1].CommitId, History[1].HistoricalPath, Callbacks, RestoreResult))) { AddError(FString::Join(RestoreResult.Errors, TEXT("\n"))); return false; }
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("Restored package bytes are readable"), FFileHelper::LoadFileToArray(RestoredBytes, *PackageFilename));
	TestTrue(TEXT("Restore writes historical package bytes"), RestoredBytes == ExpectedBytes);
	FString IndexAfter;
	TestTrue(TEXT("Index is readable after Restore"), Fixture.RunGit(TEXT("ls-files --stage -- Content/HistoryDiffFixture.uasset"), IndexAfter));
	TestEqual(TEXT("Force Restore resets the index to HEAD"), IndexAfter, IndexBefore);
	if (!RestoreResult.bSucceeded || !RestoreResult.bReloadSucceeded)
	{
		return false;
	}

	UPackage* LoadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!TestNotNull(TEXT("Direct Restore target can be loaded before public Discard"), LoadedPackage) || !TestNotNull(TEXT("Loaded public Discard target has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	UGitLocalSourceControlOperation* LoadedDiscard = UGitLocalSourceControlLibrary::StartDiscardTracked({ AssetObjectPath });
	if (!WaitForOperation(*this, LoadedDiscard, TEXT("StartDiscardTracked clean loaded package"))) return false;
	TestTrue(TEXT("StartDiscardTracked clean loaded package succeeds"), LoadedDiscard->GetResult().bSucceeded);
	TestTrue(TEXT("StartDiscardTracked clean loaded package reload succeeds"), LoadedDiscard->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("StartDiscardTracked reloads the formerly loaded package"), LoadedPackage) || !TestNotNull(TEXT("StartDiscardTracked reloaded package has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	UGitLocalSourceControlOperation* LoadedRestore = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!WaitForOperation(*this, LoadedRestore, TEXT("StartRestoreRevision clean loaded package"))) return false;
	TestTrue(TEXT("StartRestoreRevision clean loaded package succeeds"), LoadedRestore->GetResult().bSucceeded);
	TestTrue(TEXT("StartRestoreRevision clean loaded package reload succeeds"), LoadedRestore->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("StartRestoreRevision reloads the formerly loaded package"), LoadedPackage) || !TestNotNull(TEXT("StartRestoreRevision reloaded package has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(true);
	UGitLocalSourceControlOperation* DirtyLoadedRestore = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!WaitForOperation(*this, DirtyLoadedRestore, TEXT("StartRestoreRevision dirty loaded package force restore"))) return false;
	TestTrue(TEXT("StartRestoreRevision force-restores a dirty loaded package"), DirtyLoadedRestore->GetResult().bSucceeded);
	TestTrue(TEXT("StartRestoreRevision dirty loaded package reload succeeds"), DirtyLoadedRestore->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("Dirty loaded package reloads after force restore"), LoadedPackage) || !TestNotNull(TEXT("Dirty loaded package reload has its asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	FString ExternalPreflightError;
	TestFalse(TEXT("Standalone mutation preflight rejects external package paths"), FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(
		{ FPaths::Combine(FPaths::ProjectContentDir(), TEXT("__ExternalActors__/StandaloneGitTest.uasset")) }, {}, ExternalPreflightError));
	UGitLocalSourceControlOperation* DirtyRestore = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!WaitForOperation(*this, DirtyRestore, TEXT("StartRestoreRevision dirty worktree after unload"))) return false;
	TestTrue(TEXT("StartRestoreRevision force-restores a dirty worktree after unloading the clean package"), DirtyRestore->GetResult().bSucceeded);
	TestTrue(TEXT("StartRestoreRevision force restore reloads the formerly loaded package"), DirtyRestore->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("StartRestoreRevision force restore leaves the package reloaded"), LoadedPackage) || !TestNotNull(TEXT("StartRestoreRevision force restore reload has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	UGitLocalSourceControlOperation* CancelledDiscard = UGitLocalSourceControlLibrary::StartDiscardTracked({ AssetObjectPath });
	if (!TestTrue(TEXT("StartDiscardTracked accepts cancellation before mutation"), CancelledDiscard->Cancel()) || !WaitForOperation(*this, CancelledDiscard, TEXT("StartDiscardTracked cancelled after unloading"))) return false;
	TestTrue(TEXT("Cancelled StartDiscardTracked reports cancellation"), CancelledDiscard->GetResult().bCancelled);
	TestTrue(TEXT("Cancelled StartDiscardTracked reloads the formerly loaded package"), CancelledDiscard->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("Cancelled StartDiscardTracked leaves the package reloaded"), LoadedPackage) || !TestNotNull(TEXT("Cancelled StartDiscardTracked reload has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	UGitLocalSourceControlOperation* CancelledRestore = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!TestTrue(TEXT("StartRestoreRevision accepts cancellation before mutation"), CancelledRestore->Cancel()) || !WaitForOperation(*this, CancelledRestore, TEXT("StartRestoreRevision cancelled after unloading"))) return false;
	TestTrue(TEXT("Cancelled StartRestoreRevision reports cancellation"), CancelledRestore->GetResult().bCancelled);
	TestTrue(TEXT("Cancelled StartRestoreRevision reloads the formerly loaded package"), CancelledRestore->GetResult().bReloadSucceeded);
	LoadedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("Cancelled StartRestoreRevision leaves the package reloaded"), LoadedPackage) || !TestNotNull(TEXT("Cancelled StartRestoreRevision reload has an asset"), LoadedPackage->FindAssetInPackage())) return false;
	LoadedPackage->SetDirtyFlag(false);
	TArray<UPackage*> FinalPackagesToUnload;
	FinalPackagesToUnload.Add(LoadedPackage);
	FText FinalUnloadError;
	return TestTrue(TEXT("Public mutation lifecycle fixture unloads before cleanup"), UPackageTools::UnloadPackages(FinalPackagesToUnload, FinalUnloadError));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlStandaloneZeroImplicitGitAutomationTest, "Cthulhu.GitSourceControl.Standalone.ZeroImplicitGit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlStandaloneZeroImplicitGitAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	TestEqual(TEXT("Standalone module startup snapshot starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCountAtModuleStartup(), static_cast<uint64>(0));
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture)) return false;
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FGitSourceControlModule& Module = FModuleManager::LoadModuleChecked<FGitSourceControlModule>(TEXT("GitSourceControl"));
	static_cast<void>(Module);
	TestEqual(TEXT("The test-period lifecycle counter starts clean"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	const TArray<ISourceControlProvider*> SourceControlProviders = IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl"));
	TestFalse(TEXT("Git is not registered as an Unreal Source Control modular feature"), SourceControlProviders.ContainsByPredicate([](const ISourceControlProvider* Provider)
	{
		return Provider != nullptr && Provider->GetName() == TEXT("Git");
	}));

	const FString SandboxName = FString::Printf(TEXT("GitSourceControlAutomation_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = FString::Printf(TEXT("/Game/Developers/%s"), *SandboxName);
	const FString SandboxDirectory = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Developers"), SandboxName);
	if (!TestTrue(TEXT("Temporary project asset directory exists"), IFileManager::Get().MakeDirectory(*SandboxDirectory, true)))
	{
		return false;
	}
	UCurveFloat* CreatedCurve = nullptr;
	UCurveFloat* DuplicatedCurve = nullptr;
	TArray<FString> LifecyclePackageNames;
	ON_SCOPE_EXIT
	{
		TArray<UPackage*> PackagesToUnload;
		for (const FString& PackageName : LifecyclePackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				PackagesToUnload.Add(Package);
			}
		}
		if (PackagesToUnload.Num() > 0)
		{
			FText UnloadError;
			UPackageTools::UnloadPackages(PackagesToUnload, UnloadError);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		IFileManager::Get().DeleteDirectory(*SandboxDirectory, false, true);
	};

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	IAssetTools& AssetTools = AssetToolsModule.Get();
	UCurveFloatFactory* CurveFactory = NewObject<UCurveFloatFactory>();
	if (!TestNotNull(TEXT("Curve factory for the temporary lifecycle asset exists"), CurveFactory))
	{
		return false;
	}
	UObject* CreatedObject = AssetTools.CreateAsset(TEXT("LifecycleCreated"), PackagePath, UCurveFloat::StaticClass(), CurveFactory, FName(TEXT("GitSourceControlStandaloneZeroImplicitGit")));
	CreatedCurve = Cast<UCurveFloat>(CreatedObject);
	if (!TestNotNull(TEXT("AssetTools creates the temporary lifecycle asset"), CreatedCurve))
	{
		return false;
	}
	LifecyclePackageNames.Add(CreatedCurve->GetOutermost()->GetName());
	TestEqual(TEXT("Create does not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	CreatedCurve->FloatCurve.AddKey(0.0f, 1.0f);
	const FString CreatedFilename = FPackageName::LongPackageNameToFilename(CreatedCurve->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!SaveCurvePackage(*this, CreatedCurve->GetOutermost(), CreatedCurve, CreatedFilename, TEXT("created")))
	{
		return false;
	}
	TestEqual(TEXT("Create and direct package save do not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));

	UObject* DuplicatedObject = AssetTools.DuplicateAsset(TEXT("LifecycleDuplicate"), PackagePath, CreatedCurve);
	DuplicatedCurve = Cast<UCurveFloat>(DuplicatedObject);
	if (!TestNotNull(TEXT("AssetTools duplicates the temporary lifecycle asset"), DuplicatedCurve))
	{
		return false;
	}
	LifecyclePackageNames.Add(DuplicatedCurve->GetOutermost()->GetName());
	TestEqual(TEXT("Duplicate does not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	const FString DuplicatedFilename = FPackageName::LongPackageNameToFilename(DuplicatedCurve->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!SaveCurvePackage(*this, DuplicatedCurve->GetOutermost(), DuplicatedCurve, DuplicatedFilename, TEXT("duplicated")))
	{
		return false;
	}
	TestEqual(TEXT("Duplicate and direct package save do not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));

	TArray<FAssetRenameData> RenameData;
	RenameData.Add(FAssetRenameData(TWeakObjectPtr<UObject>(DuplicatedCurve), PackagePath, TEXT("LifecycleMoved")));
	if (!TestTrue(TEXT("AssetTools moves the temporary lifecycle asset"), AssetTools.RenameAssets(RenameData)))
	{
		return false;
	}
	LifecyclePackageNames.Add(DuplicatedCurve->GetOutermost()->GetName());
	TestEqual(TEXT("Move does not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	const FString MovedFilename = FPackageName::LongPackageNameToFilename(DuplicatedCurve->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!SaveCurvePackage(*this, DuplicatedCurve->GetOutermost(), DuplicatedCurve, MovedFilename, TEXT("moved")))
	{
		return false;
	}
	TestEqual(TEXT("Move and direct package save do not launch Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));

	FString GitBinary;
	FString RepositoryRoot;
	FString ResolveError;
	TestTrue(TEXT("Explicit standalone repository discovery succeeds"), GitSourceControlUtils::ResolveStandaloneRepositoryForFile(Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt")), GitBinary, RepositoryRoot, ResolveError));
	return TestTrue(TEXT("Only explicit standalone discovery starts Git"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount() > 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsObjectVerificationAutomationTest, "Cthulhu.GitSourceControl.Local.LfsObjectVerification", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsObjectVerificationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty())) return false;
	const FString Directory = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitSourceControlLfsTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!IFileManager::Get().MakeDirectory(*Directory, true)) return false;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
	const FString ObjectFilename = FPaths::Combine(Directory, TEXT("object.bin"));
	if (!FFileHelper::SaveStringToFile(TEXT("valid local LFS payload\n"), *ObjectFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	int32 ReturnCode = INDEX_NONE;
	FString PointerText;
	FString PointerError;
	FPlatformProcess::ExecProcess(*GitBinary, *FString::Printf(TEXT("lfs pointer --file=%s"), *QuoteGitArgument(ObjectFilename)), &ReturnCode, &PointerText, &PointerError);
	FString Oid;
	int64 Size = -1;
	if (!TestTrue(TEXT("LFS pointer metadata parses"), ReturnCode == 0 && ParseLfsPointer(PointerText, Oid, Size))) { AddError(PointerError); return false; }
	FString VerifyError;
	if (!TestTrue(TEXT("Valid local LFS object verifies"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, Directory, ObjectFilename, Oid, Size, VerifyError))) { AddError(VerifyError); return false; }
	if (!FFileHelper::SaveStringToFile(TEXT("valid local LFS payloxd\n"), *ObjectFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	return TestFalse(TEXT("Same-size corrupt local LFS object is rejected"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, Directory, ObjectFilename, Oid, Size, VerifyError));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsRevisionFetchAutomationTest, "Cthulhu.GitSourceControl.Local.LfsRevisionFetch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsRevisionFetchAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty()) || !IsGitLfsAvailable(GitBinary))
	{
		AddWarning(TEXT("Git LFS is unavailable; skipping the local bare LFS fetch fixture."));
		return true;
	}
	const FString Root = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitSourceControlLfsFetchTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!IFileManager::Get().MakeDirectory(*Root, true)) return false;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString Origin = FPaths::Combine(Root, TEXT("origin.git"));
	const FString Source = FPaths::Combine(Root, TEXT("source"));
	const FString Clone = FPaths::Combine(Root, TEXT("clone"));
	FString Output;
	if (!RunGitAt(*this, GitBinary, Root, FString::Printf(TEXT("init --bare %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, GitBinary, Root, FString::Printf(TEXT("init %s"), *QuoteGitArgument(Source)), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("config user.name \"GitSourceControlTests\""), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("config user.email \"git-source-control-tests@example.invalid\""), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("lfs install --local"), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("lfs track \"*.bin\""), Output))
	{
		return false;
	}
	const FString Payload = FPaths::Combine(Source, TEXT("Content/LfsPayload.bin"));
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Payload), true)
		|| !FFileHelper::SaveStringToFile(TEXT("LFS revision payload\n"), *Payload, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("add .gitattributes Content/LfsPayload.bin"), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("commit --no-gpg-sign -m \"LFS fetch fixture\""), Output)
		|| !RunGitAt(*this, GitBinary, Source, FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("push origin HEAD"), Output))
	{
		return false;
	}
	FString Branch;
	if (!RunGitAt(*this, GitBinary, Source, TEXT("branch --show-current"), Branch)) return false;
	Branch.TrimStartAndEndInline();
	if (Branch.IsEmpty() || !RunGitAt(*this, GitBinary, Origin, FString::Printf(TEXT("symbolic-ref HEAD refs/heads/%s"), *Branch), Output)
		|| !RunGitAt(*this, GitBinary, Root, FString::Printf(TEXT("clone %s %s"), *QuoteGitArgument(Origin), *QuoteGitArgument(Clone)), Output))
	{
		return false;
	}
	FString CommitId;
	FString PointerText;
	if (!RunGitAt(*this, GitBinary, Clone, TEXT("rev-parse HEAD"), CommitId) || !RunGitAt(*this, GitBinary, Clone, TEXT("show HEAD:Content/LfsPayload.bin"), PointerText)) return false;
	CommitId.TrimStartAndEndInline();
	FString Oid;
	int64 Size = -1;
	if (!TestTrue(TEXT("Bare LFS fixture pointer parses"), ParseLfsPointer(PointerText, Oid, Size))) return false;
	const FString ObjectFilename = FPaths::Combine(Clone, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
	if (!TestTrue(TEXT("Clone has the initial local LFS object"), IFileManager::Get().FileExists(*ObjectFilename)) || !TestTrue(TEXT("Remove local LFS object before fetch"), IFileManager::Get().Delete(*ObjectFilename, false, true))) return false;
	FString FetchError;
	if (!TestTrue(TEXT("A sole upstream remote permits precise historical LFS fetch"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, TEXT("Content/LfsPayload.bin"), FetchError)))
	{
		AddError(FetchError);
		return false;
	}
	if (!TestTrue(TEXT("Fetched local LFS object verifies"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, Clone, ObjectFilename, Oid, Size, FetchError))
		|| !TestTrue(TEXT("Remove object before ambiguity check"), IFileManager::Get().Delete(*ObjectFilename, false, true))
		|| !RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("remote add mirror %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("config --unset branch.%s.merge"), *Branch), Output))
	{
		return false;
	}
	FetchError.Reset();
	TestFalse(TEXT("A branch remote without branch merge is not an upstream"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, TEXT("Content/LfsPayload.bin"), FetchError));
	TestTrue(TEXT("Incomplete upstream configuration keeps the LFS object absent"), !IFileManager::Get().FileExists(*ObjectFilename));
	if (!RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("config --unset branch.%s.remote"), *Branch), Output))
	{
		return false;
	}
	FetchError.Reset();
	TestFalse(TEXT("Multiple remotes without an upstream are rejected"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, TEXT("Content/LfsPayload.bin"), FetchError));
	TestTrue(TEXT("Remote ambiguity keeps the LFS object absent"), !IFileManager::Get().FileExists(*ObjectFilename));
	TestTrue(TEXT("Remote ambiguity reports a configuration error"), FetchError.Contains(TEXT("no remote could be selected"), ESearchCase::IgnoreCase));
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	CancellationContext->Cancel();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
	FetchError.Reset();
	TestFalse(TEXT("Pre-cancelled LFS fetch is rejected"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, TEXT("Content/LfsPayload.bin"), FetchError));
	TestEqual(TEXT("Pre-cancelled LFS fetch starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	return TestTrue(TEXT("Pre-cancelled LFS fetch reports cancellation"), FetchError.Contains(TEXT("cancel"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsLiteralPathValidationAutomationTest, "Cthulhu.GitSourceControl.Local.LfsLiteralPathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsLiteralPathValidationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty())) return false;
	const TArray<FString> InvalidPaths = { TEXT("Content/*.uasset"), TEXT("Content/Asset,Other.uasset"), TEXT("Content/[Asset].uasset"), TEXT("Content/!Asset.uasset"), TEXT("Content/#Asset.uasset"), TEXT("Content\\Asset.uasset"), TEXT("Content/../Asset.uasset"), TEXT("Content//Asset.uasset") };
	for (const FString& InvalidPath : InvalidPaths)
	{
		GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
		FString Error;
		TestFalse(FString::Printf(TEXT("LFS invalid literal path is rejected: %s"), *InvalidPath), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, FPaths::ProjectDir(), TEXT("0123456789012345678901234567890123456789"), InvalidPath, Error));
		TestEqual(FString::Printf(TEXT("Invalid path launches no Git process: %s"), *InvalidPath), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlLfsNonGameThreadCancellationAutomationTest, "Cthulhu.GitSourceControl.Local.LfsNonGameThreadCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLfsNonGameThreadCancellationAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Git executable is available"), !GitBinary.IsEmpty())) return false;
	TFuture<bool> Future = Async(EAsyncExecution::ThreadPool, [GitBinary]()
	{
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> Context = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		Context->Cancel();
		GitSourceControlUtils::FGitOperationCancellationScope Scope(Context);
		FString Error;
		return !GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, FPaths::ProjectDir(), TEXT("0123456789012345678901234567890123456789"), TEXT("Content/LfsPayload.bin"), Error);
	});
	TestTrue(TEXT("Pre-cancelled non-game-thread LFS request completes promptly"), Future.WaitFor(FTimespan::FromSeconds(2.0)));
	return !Future.IsReady() || TestTrue(TEXT("Pre-cancelled non-game-thread LFS request is rejected"), Future.Get());
}

#endif

IMPLEMENT_MODULE(FDefaultModuleImpl, GitSourceControlTests)
