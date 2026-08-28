// Copyright (c) 2026

#include "GitSourceControlAssetOperations.h"
#include "GitLocalSourceControl.h"
#include "GitLocalSourceControlOperationTestReceiver.h"
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
	bool WaitForStartupGitCapability(FAutomationTestBase& Test, GitSourceControlUtils::FGitStartupCapability& OutCapability)
	{
		for (int32 Attempt = 0; Attempt < 500; ++Attempt)
		{
			OutCapability = GitSourceControlUtils::GetStartupGitCapability();
			if (OutCapability.State != GitSourceControlUtils::EGitStartupCapabilityState::Pending)
			{
				return true;
			}
			FPlatformProcess::SleepNoStats(0.01f);
		}
		Test.AddError(TEXT("Timed out waiting for the one-shot Git startup capability probe."));
		return false;
	}

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
			GitLocalSourceControl::Testing::PumpOperations();
			FPlatformProcess::SleepNoStats(0.01f);
		}
		return Test.TestTrue(*FString::Printf(TEXT("%s reaches a terminal state"), *Label), Operation->IsTerminal());
	}

	bool WaitForManagedOperationsToSettle(FAutomationTestBase& Test, const FString& Label)
	{
		const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(15.0);
		while (GitLocalSourceControl::Testing::GetManagedOperationCount() > 0 && FDateTime::UtcNow() < Deadline)
		{
			GitLocalSourceControl::Testing::PumpOperations();
			FPlatformProcess::SleepNoStats(0.01f);
		}
		GitLocalSourceControl::Testing::PumpOperations();
		return Test.TestEqual(*FString::Printf(TEXT("%s leaves no managed operations"), *Label), GitLocalSourceControl::Testing::GetManagedOperationCount(), 0);
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
	const FName BlueprintTypeMetadata(TEXT("BlueprintType"));
	const FName ForceAngelscriptBindMetadata(TEXT("ForceAngelscriptBind"));
	const FName ScriptCallableMetadata(TEXT("ScriptCallable"));
	for (UScriptStruct* Struct : { FGitLocalSourceControlProviderInfo::StaticStruct(), FGitLocalSourceControlHistoryEntry::StaticStruct(), FGitLocalSourceControlOperationResult::StaticStruct() })
	{
		TestFalse(FString::Printf(TEXT("%s remains outside the Blueprint type surface"), *Struct->GetName()), Struct->HasMetaData(BlueprintTypeMetadata));
		TestTrue(FString::Printf(TEXT("%s force-admits to AngelScript binding"), *Struct->GetName()), Struct->HasMetaData(ForceAngelscriptBindMetadata));
	}
	for (const FName FunctionName : { GET_FUNCTION_NAME_CHECKED(UGitLocalSourceControlOperation, Cancel), GET_FUNCTION_NAME_CHECKED(UGitLocalSourceControlOperation, ScheduleDiscardTrackedAfterCompletion), GET_FUNCTION_NAME_CHECKED(UGitLocalSourceControlLibrary, StartLoadHistory) })
	{
		const UFunction* Function = FunctionName == GET_FUNCTION_NAME_CHECKED(UGitLocalSourceControlLibrary, StartLoadHistory)
			? UGitLocalSourceControlLibrary::StaticClass()->FindFunctionByName(FunctionName)
			: UGitLocalSourceControlOperation::StaticClass()->FindFunctionByName(FunctionName);
		TestTrue(FString::Printf(TEXT("%s remains callable from AngelScript"), *FunctionName.ToString()), Function != nullptr && Function->HasMetaData(ScriptCallableMetadata));
	}
	const FGitLocalSourceControlProviderInfo ProviderInfo = UGitLocalSourceControlLibrary::GetProviderInfo(AssetObjectPath);
	TestTrue(TEXT("Per-asset provider info resolves the mounted fixture repository"), ProviderInfo.bAvailable);
	TestTrue(TEXT("Per-asset provider info returns the nearest fixture repository"), FPaths::IsSamePath(ProviderInfo.RepositoryRoot, Fixture.GetDirectory()));
	TestTrue(TEXT("Operation has no public manual Tick reflection method"), UGitLocalSourceControlOperation::StaticClass()->FindFunctionByName(TEXT("Tick")) == nullptr);
	TestTrue(TEXT("The module startup operation pump remains registered while idle"), GitLocalSourceControl::Testing::HasOperationTicker());
	GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	UGitLocalSourceControlOperation* LocalHitFetch = UGitLocalSourceControlLibrary::StartFetchLfsRevision(AssetObjectPath, History[1].CommitId);
	if (!TestNotNull(TEXT("A read-only LFS fetch returns an operation"), LocalHitFetch)) return false;
	TestEqual(TEXT("A non-terminal operation is held by the module manager"), GitLocalSourceControl::Testing::GetManagedOperationCount(), 1);
	TestTrue(TEXT("The fixed module operation pump remains registered while active"), GitLocalSourceControl::Testing::HasOperationTicker());
	TestFalse(TEXT("A successful read-only LFS fetch cannot arm destructive deferred cleanup"), LocalHitFetch->ScheduleDiscardTrackedAfterCompletion({ AssetObjectPath }));
	if (!WaitForOperation(*this, LocalHitFetch, TEXT("StartFetchLfsRevision local cache hit"))) return false;
	TestTrue(TEXT("StartFetchLfsRevision local cache hit succeeds"), LocalHitFetch->GetResult().bSucceeded);
	TestEqual(TEXT("A successful read-only LFS fetch never launches deferred cleanup"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 0);
	TestEqual(TEXT("A terminal operation is released by the module manager"), GitLocalSourceControl::Testing::GetManagedOperationCount(), 0);
	TestTrue(TEXT("The idle module operation pump remains registered"), GitLocalSourceControl::Testing::HasOperationTicker());
	UGitLocalSourceControlOperation* BlockedReadOnlyOperation = GitLocalSourceControl::Testing::StartBlockedReadOnlyOperationForTesting();
	if (!TestNotNull(TEXT("A blocked read-only operation returns an operation"), BlockedReadOnlyOperation)) return false;
	bool bBlockedReadOnlyOperationReleased = false;
	ON_SCOPE_EXIT
	{
		if (!bBlockedReadOnlyOperationReleased)
		{
			GitLocalSourceControl::Testing::ReleaseBlockedReadOnlyOperation();
		}
	};
	if (!TestTrue(TEXT("The blocked read-only worker reaches its final-publication test gate"), GitLocalSourceControl::Testing::WaitForBlockedReadOnlyOperationToReachFinalPublication())) return false;
	TWeakObjectPtr<UGitLocalSourceControlOperation> WeakBlockedReadOnlyOperation = BlockedReadOnlyOperation;
	BlockedReadOnlyOperation = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestTrue(TEXT("The FGCObject manager keeps a caller-unreferenced non-terminal operation alive through GC"), WeakBlockedReadOnlyOperation.IsValid())) return false;
	GitLocalSourceControl::Testing::ReleaseBlockedReadOnlyOperation();
	bBlockedReadOnlyOperationReleased = true;
	BlockedReadOnlyOperation = WeakBlockedReadOnlyOperation.Get();
	if (!WaitForOperation(*this, BlockedReadOnlyOperation, TEXT("Blocked read-only operation after caller reference is dropped"))) return false;
	TestTrue(TEXT("The caller-unreferenced operation completes after its worker is released"), BlockedReadOnlyOperation->GetResult().bSucceeded);
	{
		UGitLocalSourceControlOperation* CancelAtFinalPublication = GitLocalSourceControl::Testing::StartBlockedReadOnlyOperationForTesting();
		if (!TestNotNull(TEXT("A cancellation-race operation returns an operation"), CancelAtFinalPublication)) return false;
		bool bCancelAtFinalPublicationReleased = false;
		ON_SCOPE_EXIT
		{
			if (!bCancelAtFinalPublicationReleased)
			{
				GitLocalSourceControl::Testing::ReleaseBlockedReadOnlyOperation();
			}
		};
		if (!TestTrue(TEXT("The cancellation-race worker reaches final publication before result lock"), GitLocalSourceControl::Testing::WaitForBlockedReadOnlyOperationToReachFinalPublication())) return false;
		if (!TestTrue(TEXT("Cancel succeeds while the worker is paused before final result publication"), CancelAtFinalPublication->Cancel())) return false;
		GitLocalSourceControl::Testing::ReleaseBlockedReadOnlyOperation();
		bCancelAtFinalPublicationReleased = true;
		if (!WaitForOperation(*this, CancelAtFinalPublication, TEXT("Cancellation-race operation"))) return false;
		TestTrue(TEXT("A successful Cancel linearizes the terminal result as cancelled"), CancelAtFinalPublication->GetResult().bCancelled);
		TestFalse(TEXT("A successful Cancel cannot publish a successful terminal result"), CancelAtFinalPublication->GetResult().bSucceeded);
		TestEqual(TEXT("A successful Cancel reaches the Cancelled terminal phase"), CancelAtFinalPublication->GetPhase(), EGitLocalSourceControlOperationPhase::Cancelled);
	}
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
	GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
	UGitLocalSourceControlOperation* FailedDeferredParent = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, TEXT("0000000000000000000000000000000000000000"));
	if (!TestNotNull(TEXT("A parent that fails before mutation returns an operation"), FailedDeferredParent)
		|| !TestTrue(TEXT("A pre-armed cleanup accepts a parent that later fails before mutation"), FailedDeferredParent->ScheduleDiscardTrackedAfterCompletion({ AssetObjectPath }))
		|| !WaitForOperation(*this, FailedDeferredParent, TEXT("StartRestoreRevision failed parent with pre-armed cleanup"))) return false;
	TestFalse(TEXT("The invalid parent revision does not mutate disk"), FailedDeferredParent->GetResult().bSucceeded);
	TestEqual(TEXT("A failed parent never starts its deferred cleanup"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 0);
	if (!WaitForManagedOperationsToSettle(*this, TEXT("A failed parent"))) return false;

	{
		GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
		TArray<uint8> RollbackBaselineBytes;
		if (!TestTrue(TEXT("The rollback parent baseline bytes are readable"), FFileHelper::LoadFileToArray(RollbackBaselineBytes, *PackageFilename))) return false;
		FString RollbackBaselineStatus;
		if (!Fixture.RunGit(TEXT("status --porcelain -- Content/HistoryDiffFixture.uasset"), RollbackBaselineStatus)) return false;
		GitLocalSourceControl::Testing::SetForceRestoreWorktreeRollback(true);
		ON_SCOPE_EXIT { GitLocalSourceControl::Testing::SetForceRestoreWorktreeRollback(false); };
		UGitLocalSourceControlOperation* RolledBackDeferredParent = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
		if (!TestNotNull(TEXT("A rollback parent returns an operation"), RolledBackDeferredParent)
			|| !TestTrue(TEXT("A rollback parent accepts pre-armed deferred cleanup"), RolledBackDeferredParent->ScheduleDiscardTrackedAfterCompletion({ AssetObjectPath }))
			|| !WaitForOperation(*this, RolledBackDeferredParent, TEXT("StartRestoreRevision forced rollback with pre-armed cleanup"))) return false;
		TestFalse(TEXT("The forced worktree replacement failure rolls the parent back"), RolledBackDeferredParent->GetResult().bSucceeded);
		TestTrue(TEXT("The forced parent failure reports rollback"), RolledBackDeferredParent->GetResult().Errors.ContainsByPredicate([](const FString& Error)
		{
			return Error.Contains(TEXT("rollback"), ESearchCase::IgnoreCase);
		}));
		TestEqual(TEXT("A rolled-back parent never starts its deferred cleanup"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 0);
		if (!WaitForManagedOperationsToSettle(*this, TEXT("A rolled-back parent"))) return false;
		TArray<uint8> RollbackResultBytes;
		if (!TestTrue(TEXT("The rollback parent result bytes are readable"), FFileHelper::LoadFileToArray(RollbackResultBytes, *PackageFilename))) return false;
		TestTrue(TEXT("The failed parent restores the exact pre-mutation worktree bytes before deferred cleanup is skipped"), RollbackResultBytes == RollbackBaselineBytes);
		FString RollbackStatus;
		if (!Fixture.RunGit(TEXT("status --porcelain -- Content/HistoryDiffFixture.uasset"), RollbackStatus)) return false;
		TestEqual(TEXT("The failed parent restores the pre-mutation index and worktree status before deferred cleanup is skipped"), RollbackStatus, RollbackBaselineStatus);
	}

	GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
	UGitLocalSourceControlOperation* SuccessfulDeferredParent = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!TestNotNull(TEXT("A successful parent returns an operation"), SuccessfulDeferredParent)
		|| !TestTrue(TEXT("A successful parent accepts pre-armed deferred cleanup"), SuccessfulDeferredParent->ScheduleDiscardTrackedAfterCompletion({ AssetObjectPath }))
		|| !TestEqual(TEXT("The shutdown-style drain starts with the managed parent operation"), GitLocalSourceControl::Testing::GetManagedOperationCount(), 1)
		|| !TestTrue(TEXT("A shutdown-style drain removes the fixed ticker, drains parent and child, then restores the pump"), GitLocalSourceControl::Testing::DrainOperationsForTesting())) return false;
	TestTrue(TEXT("The shutdown-style drain terminalizes the successful parent"), SuccessfulDeferredParent->IsTerminal());
	TestTrue(TEXT("The successful parent changes disk before scheduling cleanup"), SuccessfulDeferredParent->GetResult().bSucceeded);
	TestEqual(TEXT("The shutdown-style drain releases parent and internal child from the manager"), GitLocalSourceControl::Testing::GetManagedOperationCount(), 0);
	TestTrue(TEXT("The fixed ticker is registered again after the shutdown-style drain"), GitLocalSourceControl::Testing::HasOperationTicker());
	TestEqual(TEXT("A successful parent starts its deferred cleanup exactly once"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 1);
	FString DeferredCleanupStatus;
	if (!Fixture.RunGit(TEXT("status --porcelain -- Content/HistoryDiffFixture.uasset"), DeferredCleanupStatus)) return false;
	DeferredCleanupStatus.TrimStartAndEndInline();
	TestTrue(TEXT("Successful deferred cleanup returns the worktree to HEAD"), DeferredCleanupStatus.IsEmpty());

	GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
	GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(true);
	UGitLocalSourceControlOperation* ReloadFailureDeferredParent = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!TestNotNull(TEXT("A reload-failure parent returns an operation"), ReloadFailureDeferredParent)
		|| !TestTrue(TEXT("A reload-failure parent accepts pre-armed deferred cleanup"), ReloadFailureDeferredParent->ScheduleDiscardTrackedAfterCompletion({ AssetObjectPath }))
		|| !WaitForOperation(*this, ReloadFailureDeferredParent, TEXT("StartRestoreRevision reload failure with deferred cleanup")))
	{
		GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
		return false;
	}
	GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
	TestFalse(TEXT("A post-mutation reload failure makes the parent fail"), ReloadFailureDeferredParent->GetResult().bSucceeded);
	TestFalse(TEXT("A post-mutation reload failure is reported on the parent"), ReloadFailureDeferredParent->GetResult().bReloadSucceeded);
	if (!WaitForManagedOperationsToSettle(*this, TEXT("A reload-failure parent and its cleanup"))) return false;
	TestEqual(TEXT("A reload-failure parent still starts deferred cleanup exactly once"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 1);
	FString ReloadFailureDeferredStatus;
	if (!Fixture.RunGit(TEXT("status --porcelain -- Content/HistoryDiffFixture.uasset"), ReloadFailureDeferredStatus)) return false;
	ReloadFailureDeferredStatus.TrimStartAndEndInline();
	TestTrue(TEXT("Deferred cleanup returns disk to HEAD after parent reload failure"), ReloadFailureDeferredStatus.IsEmpty());

	GitLocalSourceControl::Testing::ClearLastDeferredCleanupDiagnostic();
	GitLocalSourceControl::Testing::ResetDeferredCleanupLaunchCount();
	AddExpectedErrorPlain(TEXT("Deferred Git cleanup failed for asset paths [/Invalid/DeferredCleanup.DeferredCleanup]"), EAutomationExpectedErrorFlags::Contains, 1);
	UGitLocalSourceControlOperation* FailedDeferredChildParent = UGitLocalSourceControlLibrary::StartRestoreRevision(AssetObjectPath, History[1].CommitId);
	if (!TestNotNull(TEXT("A parent with a failing cleanup returns an operation"), FailedDeferredChildParent)
		|| !TestTrue(TEXT("A parent accepts an invalid deferred cleanup target for failure handling"), FailedDeferredChildParent->ScheduleDiscardTrackedAfterCompletion({ TEXT("/Invalid/DeferredCleanup.DeferredCleanup") }))
		|| !WaitForOperation(*this, FailedDeferredChildParent, TEXT("StartRestoreRevision parent with failing deferred cleanup"))) return false;
	TestTrue(TEXT("The parent succeeds before the internal cleanup failure"), FailedDeferredChildParent->GetResult().bSucceeded);
	if (!WaitForManagedOperationsToSettle(*this, TEXT("The failing internal cleanup"))) return false;
	TestEqual(TEXT("A parent starts the failing internal cleanup exactly once"), GitLocalSourceControl::Testing::GetDeferredCleanupLaunchCount(), 1);
	const FString DeferredCleanupDiagnostic = GitLocalSourceControl::Testing::GetLastDeferredCleanupDiagnostic();
	TestTrue(TEXT("A failed internal deferred cleanup reports its asset path"), DeferredCleanupDiagnostic.Contains(TEXT("/Invalid/DeferredCleanup.DeferredCleanup")));
	TestTrue(TEXT("A failed internal deferred cleanup gives an actionable recovery instruction"), DeferredCleanupDiagnostic.Contains(TEXT("inspect these assets"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("A failed internal deferred cleanup is released by the manager"), GitLocalSourceControl::Testing::GetManagedOperationCount(), 0);
	TestTrue(TEXT("A failed internal deferred cleanup leaves the fixed module pump registered"), GitLocalSourceControl::Testing::HasOperationTicker());

	GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(true);
	UGitLocalSourceControlOperation* ReloadFailure = UGitLocalSourceControlLibrary::StartDiscardTracked({ AssetObjectPath });
	if (!TestNotNull(TEXT("A reload-failure operation exists"), ReloadFailure))
	{
		GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
		return false;
	}
	UGitLocalSourceControlOperationTestReceiver* ReloadFailureReceiver = NewObject<UGitLocalSourceControlOperationTestReceiver>();
	if (!TestNotNull(TEXT("A reload-failure operation event receiver exists"), ReloadFailureReceiver))
	{
		GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
		return false;
	}
	ReloadFailure->OnProgress.AddDynamic(ReloadFailureReceiver, &UGitLocalSourceControlOperationTestReceiver::HandleProgress);
	ReloadFailure->OnCompleted.AddDynamic(ReloadFailureReceiver, &UGitLocalSourceControlOperationTestReceiver::HandleCompleted);
	if (!WaitForOperation(*this, ReloadFailure, TEXT("StartDiscardTracked forced package reload failure")))
	{
		GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
		return false;
	}
	GitLocalSourceControl::Testing::SetForcePreparedPackageReloadFailure(false);
	TestFalse(TEXT("A delayed package reload failure makes the operation fail"), ReloadFailure->GetResult().bSucceeded);
	TestFalse(TEXT("A delayed package reload failure is reported"), ReloadFailure->GetResult().bReloadSucceeded);
	TestEqual(TEXT("A delayed package reload failure reaches Failed"), ReloadFailure->GetPhase(), EGitLocalSourceControlOperationPhase::Failed);
	TestTrue(TEXT("The reload-failure operation broadcasts its worker Reloading phase"), ReloadFailureReceiver->ProgressPhases.Contains(EGitLocalSourceControlOperationPhase::Reloading));
	TestEqual(TEXT("The reload-failure operation broadcasts exactly one terminal progress phase"), ReloadFailureReceiver->TerminalProgressCount, 1);
	TestEqual(TEXT("The reload-failure operation broadcasts completion exactly once"), ReloadFailureReceiver->CompletedCount, 1);
	TestTrue(TEXT("Completion is broadcast after terminal progress following package reload"), ReloadFailureReceiver->bCompletedAfterTerminalProgress);
	TestFalse(TEXT("Completion observes the final package reload failure"), ReloadFailureReceiver->CompletedResult.bReloadSucceeded);
	GitLocalSourceControl::Testing::PumpOperations();
	GitLocalSourceControl::Testing::PumpOperations();
	TestEqual(TEXT("Additional manager pumps do not repeat completion"), ReloadFailureReceiver->CompletedCount, 1);
	TestTrue(TEXT("A delayed package reload failure explains the partial disk mutation"), ReloadFailure->GetResult().Errors.ContainsByPredicate([](const FString& Error)
	{
		return Error.Contains(TEXT("changed the asset file on disk"));
	}));
	LoadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!TestNotNull(TEXT("The force-failed package can be recovered after the test seam is cleared"), LoadedPackage)) return false;
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
	GitSourceControlUtils::FGitStartupCapability StartupCapability;
	if (!WaitForStartupGitCapability(*this, StartupCapability))
	{
		return false;
	}
	TestEqual(TEXT("Standalone module launches exactly one startup Git capability probe"),
		GitSourceControlUtils::Testing::GetStartupGitCapabilityProbeCount(), static_cast<uint32>(1));
	if (!TestTrue(TEXT("Startup Git capability probe finds a supported Git release"),
		StartupCapability.State == GitSourceControlUtils::EGitStartupCapabilityState::Available))
	{
		AddError(StartupCapability.Diagnostic);
		return false;
	}
	FGitTestFixture Fixture(*this);
	if (!Fixture.Initialize() || !CreateCommittedFixture(Fixture)) return false;
	// The executable path is frozen by startup. Reset only process accounting so
	// normal asset lifecycle checks prove there is no additional Git activity.
	GitSourceControlUtils::Testing::ResetVerifiedGitBinaryCache();
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
	TestTrue(TEXT("Explicit standalone repository resolution succeeds"), GitSourceControlUtils::ResolveStandaloneRepositoryForFile(Fixture.AbsoluteFilename(TEXT("Content/Tracked.txt")), GitBinary, RepositoryRoot, ResolveError));
	TestTrue(TEXT("Explicit repository resolution reuses the frozen startup Git binary"), FPaths::IsSamePath(GitBinary, StartupCapability.GitBinary));
	return TestEqual(TEXT("Explicit repository resolution starts no additional Git process after startup"),
		GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlStartupCapabilityGateAutomationTest, "Cthulhu.GitSourceControl.Standalone.StartupCapabilityGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlStartupCapabilityGateAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const GitSourceControlUtils::FGitStartupCapability SavedCapability = GitSourceControlUtils::GetStartupGitCapability();
	ON_SCOPE_EXIT
	{
		GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(SavedCapability);
	};

	GitSourceControlUtils::FGitStartupCapability PendingCapability;
	PendingCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Pending;
	PendingCapability.Diagnostic = TEXT("Checking for Git 2.53.0 or newer...");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(PendingCapability);
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FString GitBinary;
	FString RepositoryRoot;
	FString Error;
	TestFalse(TEXT("Pending startup capability blocks Git action repository resolution"),
		GitSourceControlUtils::ResolveStandaloneRepositoryForFile(FPaths::GetProjectFilePath(), GitBinary, RepositoryRoot, Error));
	TestEqual(TEXT("Pending Git action capability gate starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	TestTrue(TEXT("Pending startup capability reports an actionable explanation"), Error.Contains(TEXT("Git 2.53.0")));

	GitSourceControlUtils::FGitStartupCapability UnavailableCapability;
	UnavailableCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Unavailable;
	UnavailableCapability.Diagnostic = TEXT("Detected Git executable: C:/Tools/Git/bin/git.exe\nDetected version: git version 2.42.0\nGit 2.53.0 or a newer release is required. Install or upgrade Git, then restart the Editor.");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(UnavailableCapability);
	Error.Reset();
	TestFalse(TEXT("Unavailable startup capability blocks Git action repository resolution"),
		GitSourceControlUtils::ResolveStandaloneRepositoryForFile(FPaths::GetProjectFilePath(), GitBinary, RepositoryRoot, Error));
	TestEqual(TEXT("Unavailable Git action capability gate starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	TestTrue(TEXT("Unavailable startup capability preserves detected path and restart guidance"),
		Error.Contains(TEXT("C:/Tools/Git/bin/git.exe")) && Error.Contains(TEXT("restart the Editor")));
	return true;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlGitBinaryCapabilityCacheAutomationTest, "Cthulhu.GitSourceControl.Local.GitBinaryCapabilityCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlGitBinaryCapabilityCacheAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlLocalAutomationTestsPrivate;
	GitSourceControlUtils::FGitStartupCapability StartupCapability;
	if (!WaitForStartupGitCapability(*this, StartupCapability))
	{
		return false;
	}
	if (!TestTrue(TEXT("Startup capability is available before checking its frozen binary"),
		StartupCapability.State == GitSourceControlUtils::EGitStartupCapabilityState::Available))
	{
		AddError(StartupCapability.Diagnostic);
		return false;
	}
	GitSourceControlUtils::Testing::ResetVerifiedGitBinaryCache();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	const FString FirstGitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestTrue(TEXT("Frozen startup capability provides a supported Git binary"), !FirstGitBinary.IsEmpty()))
	{
		return false;
	}
	const FString CachedGitBinary = GitSourceControlUtils::FindGitBinaryPath();
	TestTrue(TEXT("Frozen startup capability returns the same Git binary"), FPaths::IsSamePath(CachedGitBinary, FirstGitBinary));
	return TestEqual(TEXT("Reading the frozen startup binary does not launch Git"),
		GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
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
	const FString HistoricalPayloadPath = TEXT("Content/Lfs Payload.bin");
	const FString Payload = FPaths::Combine(Source, HistoricalPayloadPath);
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Payload), true)
		|| !FFileHelper::SaveStringToFile(TEXT("LFS revision payload\n"), *Payload, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !RunGitAt(*this, GitBinary, Source, FString::Printf(TEXT("add .gitattributes %s"), *QuoteGitArgument(HistoricalPayloadPath)), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("commit --no-gpg-sign -m \"LFS fetch fixture\""), Output)
		|| !RunGitAt(*this, GitBinary, Source, FString::Printf(TEXT("remote add origin %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, GitBinary, Source, TEXT("push origin HEAD"), Output))
	{
		return false;
	}
	FString Branch;
	if (!RunGitAt(*this, GitBinary, Source, TEXT("branch --show-current"), Branch)) return false;
	Branch.TrimStartAndEndInline();
	const FString BranchRef = FString::Printf(TEXT("refs/heads/%s"), *Branch);
	if (Branch.IsEmpty() || !RunGitAt(*this, GitBinary, Origin, FString::Printf(TEXT("symbolic-ref HEAD %s"), *QuoteGitArgument(BranchRef)), Output)
		|| !RunGitAt(*this, GitBinary, Root, FString::Printf(TEXT("clone %s %s"), *QuoteGitArgument(Origin), *QuoteGitArgument(Clone)), Output))
	{
		return false;
	}
	FString CommitId;
	FString PointerText;
	if (!RunGitAt(*this, GitBinary, Clone, TEXT("rev-parse HEAD"), CommitId)
		|| !RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("show %s"), *QuoteGitArgument(TEXT("HEAD:Content/Lfs Payload.bin"))), PointerText)) return false;
	CommitId.TrimStartAndEndInline();
	FString Oid;
	int64 Size = -1;
	if (!TestTrue(TEXT("Bare LFS fixture pointer parses"), ParseLfsPointer(PointerText, Oid, Size))) return false;
	const FString ObjectFilename = FPaths::Combine(Clone, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
	if (!TestTrue(TEXT("Clone has the initial local LFS object"), IFileManager::Get().FileExists(*ObjectFilename)) || !TestTrue(TEXT("Remove local LFS object before fetch"), IFileManager::Get().Delete(*ObjectFilename, false, true))) return false;
	FString FetchError;
	if (!TestTrue(TEXT("A sole upstream remote permits precise historical LFS fetch with a space-containing path"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, HistoricalPayloadPath, FetchError)))
	{
		AddError(FetchError);
		return false;
	}
	if (!TestTrue(TEXT("Fetched local LFS object verifies"), GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, Clone, ObjectFilename, Oid, Size, FetchError))
		|| !TestTrue(TEXT("Remove object before ambiguity check"), IFileManager::Get().Delete(*ObjectFilename, false, true))
		|| !RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("remote add mirror %s"), *QuoteGitArgument(Origin)), Output)
		|| !RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("config --unset %s"), *QuoteGitArgument(FString::Printf(TEXT("branch.%s.merge"), *Branch))), Output))
	{
		return false;
	}
	FetchError.Reset();
	TestFalse(TEXT("A branch remote without branch merge is not an upstream"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, HistoricalPayloadPath, FetchError));
	TestTrue(TEXT("Incomplete upstream configuration keeps the LFS object absent"), !IFileManager::Get().FileExists(*ObjectFilename));
	if (!RunGitAt(*this, GitBinary, Clone, FString::Printf(TEXT("config --unset %s"), *QuoteGitArgument(FString::Printf(TEXT("branch.%s.remote"), *Branch))), Output))
	{
		return false;
	}
	FetchError.Reset();
	TestFalse(TEXT("Multiple remotes without an upstream are rejected"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, HistoricalPayloadPath, FetchError));
	TestTrue(TEXT("Remote ambiguity keeps the LFS object absent"), !IFileManager::Get().FileExists(*ObjectFilename));
	TestTrue(TEXT("Remote ambiguity reports a configuration error"), FetchError.Contains(TEXT("no remote could be selected"), ESearchCase::IgnoreCase));
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	CancellationContext->Cancel();
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
	FetchError.Reset();
	TestFalse(TEXT("Pre-cancelled LFS fetch is rejected"), GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, Clone, CommitId, HistoricalPayloadPath, FetchError));
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
