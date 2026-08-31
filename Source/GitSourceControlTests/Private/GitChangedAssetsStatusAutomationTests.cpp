// Copyright (c) 2026

#include "GitChangedAssetsStatus.h"
#include "GitMapPackageSet.h"
#include "GitSourceControlUtils.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

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

	/** 使用真实 UWorld + SavePackage 生成临时 .umap, 避免用伪二进制替代地图 fixture. */
	class FScopedMapFixture final
	{
	public:
		FScopedMapFixture(FAutomationTestBase& InTest, FFixture& InFixture)
			: Test(InTest)
			, Fixture(InFixture)
		{
		}

		~FScopedMapFixture()
		{
			if (Package != nullptr)
			{
				Package->SetDirtyFlag(false);
				TArray<UPackage*> Packages;
				Packages.Add(Package);
				FText Error;
				UPackageTools::UnloadPackages(Packages, Error, true);
			}
			if (!MountRoot.IsEmpty())
			{
				FPackageName::UnRegisterMountPoint(MountRoot, ContentRoot);
			}
		}

		bool Create()
		{
			ContentRoot = Fixture.AbsoluteFilename(TEXT("Content"));
			MountRoot = FString::Printf(TEXT("/GitChangedAssetsStatusFixture_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FPackageName::RegisterMountPoint(MountRoot, ContentRoot);
			PackageName = MountRoot + TEXT("Maps/StatusFixture");
			Package = CreatePackage(*PackageName);
			World = Package != nullptr ? UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("StatusFixture")), Package) : nullptr;
			if (World != nullptr)
			{
				World->SetFlags(RF_Public | RF_Standalone);
			}
			if (!Test.TestNotNull(TEXT("Real map fixture creates a package"), Package)
				|| !Test.TestNotNull(TEXT("Real map fixture creates a UWorld"), World))
			{
				return false;
			}
			return SaveMap(TEXT("Initial"));
		}

		bool AddRevisionActorAndSave()
		{
			if (!Test.TestNotNull(TEXT("Real map fixture world remains valid for second revision"), World))
			{
				return false;
			}
			AActor* Actor = World->SpawnActor<AActor>();
			if (!Test.TestNotNull(TEXT("Real map fixture adds a revision actor"), Actor))
			{
				return false;
			}
			Actor->SetActorLabel(TEXT("StatusRevisionActor"));
			return SaveMap(TEXT("Second"));
		}

		const FString& GetPackageFilename() const { return PackageFilename; }
		const FString& GetPackageName() const { return PackageName; }

	private:
		bool SaveMap(const TCHAR* Label)
		{
			PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			Package->MarkPackageDirty();
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true))
			{
				return Test.TestTrue(TEXT("Creates directory for real .umap fixture"), false);
			}
			return Test.TestTrue(FString::Printf(TEXT("Saves real .umap fixture (%s)"), Label),
				UPackage::SavePackage(Package, World, *PackageFilename, SaveArgs))
				&& Test.TestTrue(TEXT("Saved real .umap fixture exists on disk"), FPaths::FileExists(PackageFilename));
		}

		FAutomationTestBase& Test;
		FFixture& Fixture;
		FString MountRoot;
		FString ContentRoot;
		FString PackageName;
		FString PackageFilename;
		UPackage* Package = nullptr;
		UWorld* World = nullptr;
	};

	const FGitChangedAssetEntry* FindByPath(const TArray<FGitChangedAssetEntry>& InEntries, const FString& InRepositoryRelativePath)
	{
		return InEntries.FindByPredicate([&InRepositoryRelativePath](const FGitChangedAssetEntry& Entry)
		{
			return Entry.RepositoryRelativePath == InRepositoryRelativePath;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsStatusParserAutomationTest, "UEGitPlugin.ChangedAssets.StatusParser", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

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
	AppendNulUtf8(Output, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/ChangedMap.umap"));
	AppendNulUtf8(Output, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/RenamedMap New.umap"));
	AppendNulUtf8(Output, TEXT("Content/RenamedMap Old.umap"));
	AppendNulUtf8(Output, TEXT("? Content/UntrackedMap.umap"));
	AppendNulUtf8(Output, TEXT("1 D. N... 100644 000000 000000 1111111 0000000 Content/DeletedMap.umap"));
	AppendNulUtf8(Output, TEXT("1 A. N... 000000 100644 100644 0000000 3333333 Content/AddedMap.umap"));
	AppendNulUtf8(Output, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/ChangedMap.uexp"));
	AppendNulUtf8(Output, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/NotAnAsset.txt"));

	TArray<FGitChangedAssetEntry> Entries;
	FString Error;
	if (!TestTrue(TEXT("NUL-safe porcelain-v2 parser accepts mixed records"), FGitChangedAssetsStatus::ParsePorcelainV2(Output, FPaths::ProjectDir(), Entries, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT(".uasset and .umap records become Changed Assets entries"), Entries.Num(), 9);
	const FGitChangedAssetEntry* Modified = FindByPath(Entries, TEXT("Content/Modified.uasset"));
	const FGitChangedAssetEntry* Renamed = FindByPath(Entries, TEXT("Content/Renamed New.uasset"));
	const FGitChangedAssetEntry* Untracked = FindByPath(Entries, TEXT("Content/Untracked Asset.uasset"));
	const FGitChangedAssetEntry* Conflict = FindByPath(Entries, TEXT("Content/Conflict.uasset"));
	const FGitChangedAssetEntry* ChangedMap = FindByPath(Entries, TEXT("Content/ChangedMap.umap"));
	const FGitChangedAssetEntry* RenamedMap = FindByPath(Entries, TEXT("Content/RenamedMap New.umap"));
	const FGitChangedAssetEntry* UntrackedMap = FindByPath(Entries, TEXT("Content/UntrackedMap.umap"));
	const FGitChangedAssetEntry* DeletedMap = FindByPath(Entries, TEXT("Content/DeletedMap.umap"));
	const FGitChangedAssetEntry* AddedMap = FindByPath(Entries, TEXT("Content/AddedMap.umap"));
	if (!TestNotNull(TEXT("Modified asset exists"), Modified) || !TestNotNull(TEXT("Renamed asset exists"), Renamed)
		|| !TestNotNull(TEXT("Untracked asset exists"), Untracked) || !TestNotNull(TEXT("Conflicted asset exists"), Conflict)
		|| !TestNotNull(TEXT("Modified map exists"), ChangedMap) || !TestNotNull(TEXT("Renamed map exists"), RenamedMap)
		|| !TestNotNull(TEXT("Untracked map exists"), UntrackedMap) || !TestNotNull(TEXT("Deleted map exists"), DeletedMap)
		|| !TestNotNull(TEXT("Added map exists"), AddedMap))
	{
		return false;
	}
	TestEqual(TEXT("Modified state is aggregated"), Modified->State, EGitChangedAssetState::Modified);
	TestEqual(TEXT("Rename keeps the old NUL path"), Renamed->RenameFromRepositoryRelativePath, FString(TEXT("Content/Renamed Old.uasset")));
	TestEqual(TEXT("Rename has its raw index state"), Renamed->IndexStatus, TEXT('R'));
	TestEqual(TEXT("Untracked state is aggregated"), Untracked->State, EGitChangedAssetState::Untracked);
	TestEqual(TEXT("Conflict state is aggregated"), Conflict->State, EGitChangedAssetState::Conflicted);
	TestTrue(TEXT("Conflict is not base-revertable"), !Conflict->bBaseRevertEligible);
	TestEqual(TEXT("Modified map state is aggregated"), ChangedMap->State, EGitChangedAssetState::Modified);
	TestEqual(TEXT("Rename keeps the old NUL map path"), RenamedMap->RenameFromRepositoryRelativePath, FString(TEXT("Content/RenamedMap Old.umap")));
	TestEqual(TEXT("Rename map has its raw index state"), RenamedMap->IndexStatus, TEXT('R'));
	TestEqual(TEXT("Untracked map state is aggregated"), UntrackedMap->State, EGitChangedAssetState::Untracked);
	TestEqual(TEXT("Deleted map state is aggregated"), DeletedMap->State, EGitChangedAssetState::Deleted);
	TestEqual(TEXT("Added map state is aggregated"), AddedMap->State, EGitChangedAssetState::Added);
	TestTrue(TEXT("Map sidecar record does not become a separate Changed Assets row"), FindByPath(Entries, TEXT("Content/ChangedMap.uexp")) == nullptr);
	TArray<FGitChangedAssetEntry> EntriesWithArtifacts;
	TArray<FGitChangedAssetArtifact> Artifacts;
	Error.Reset();
	if (!TestTrue(TEXT("Parser preserves sidecars alongside primary rows"),
		FGitChangedAssetsStatus::ParsePorcelainV2WithArtifacts(Output, FPaths::ProjectDir(), EntriesWithArtifacts, Artifacts, Error)))
	{
		AddError(Error);
		return false;
	}
	const FGitChangedAssetArtifact* ParsedSidecar = Artifacts.FindByPredicate([](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == TEXT("Content/ChangedMap.uexp");
	});
	if (!TestNotNull(TEXT("Parser exposes changed map sidecar artifact"), ParsedSidecar))
	{
		return false;
	}
	TestEqual(TEXT("Sidecar artifact has sidecar kind"), ParsedSidecar->Kind, EGitChangedAssetArtifactKind::Sidecar);
	TestEqual(TEXT("Sidecar parsing keeps primary row count unchanged"), EntriesWithArtifacts.Num(), 9);
	TArray<uint8> SidecarOnlyOutput;
	AppendNulUtf8(SidecarOnlyOutput, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/SidecarOnlyMap.uexp"));
	FFixture SidecarFixture(*this);
	if (!SidecarFixture.Initialize() || !SidecarFixture.WriteFile(TEXT("Content/SidecarOnlyMap.umap"), TEXT("existing primary placeholder")))
	{
		return false;
	}
	TArray<FGitChangedAssetEntry> SidecarOnlyEntries;
	TArray<FGitChangedAssetArtifact> SidecarOnlyArtifacts;
	Error.Reset();
	if (!TestTrue(TEXT("Sidecar-only status synthesizes its primary map row"),
		FGitChangedAssetsStatus::ParsePorcelainV2WithArtifacts(SidecarOnlyOutput, SidecarFixture.GetRoot(), SidecarOnlyEntries, SidecarOnlyArtifacts, Error)))
	{
		AddError(Error);
		return false;
	}
	const FGitChangedAssetEntry* SynthesizedMap = FindByPath(SidecarOnlyEntries, TEXT("Content/SidecarOnlyMap.umap"));
	if (!TestNotNull(TEXT("Sidecar-only status exposes a selectable primary .umap"), SynthesizedMap))
	{
		return false;
	}
	if (!TestTrue(TEXT("Sidecar-only status keeps the sidecar artifact for selection closure"), SidecarOnlyArtifacts.ContainsByPredicate([](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == TEXT("Content/SidecarOnlyMap.uexp") && Artifact.Kind == EGitChangedAssetArtifactKind::Sidecar;
	}))) return false;
	FFixture RenameSidecarFixture(*this);
	if (!RenameSidecarFixture.Initialize() || !RenameSidecarFixture.WriteFile(TEXT("Content/RenamedOnly.umap"), TEXT("existing primary"))) return false;
	TArray<uint8> SidecarRenameOutput;
	AppendNulUtf8(SidecarRenameOutput, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/RenamedOnly.uexp"));
	AppendNulUtf8(SidecarRenameOutput, TEXT("Content/OldOnly.uexp"));
	TArray<FGitChangedAssetEntry> SidecarRenameEntries;
	TArray<FGitChangedAssetArtifact> SidecarRenameArtifacts;
	Error.Reset();
	if (!TestTrue(TEXT("Sidecar-only rename status parses"), FGitChangedAssetsStatus::ParsePorcelainV2WithArtifacts(SidecarRenameOutput, RenameSidecarFixture.GetRoot(), SidecarRenameEntries, SidecarRenameArtifacts, Error))) return false;
	const FGitChangedAssetEntry* SidecarRenameMap = FindByPath(SidecarRenameEntries, TEXT("Content/RenamedOnly.umap"));
	if (!TestNotNull(TEXT("Sidecar-only rename synthesizes its current map row"), SidecarRenameMap)) return false;
	TestTrue(TEXT("Sidecar-only rename remains diagnostic-only without a fabricated primary source"), !SidecarRenameMap->bCanRevert && SidecarRenameMap->RenameFromRepositoryRelativePath.IsEmpty());
	TArray<uint8> ConflictClosureOutput;
	AppendNulUtf8(ConflictClosureOutput, TEXT("1 .M N... 100644 100644 100644 1111111 2222222 Content/ConflictClosure.umap"));
	AppendNulUtf8(ConflictClosureOutput, TEXT("u UU N... 100644 100644 100644 100644 1111111 2222222 3333333 Content/ConflictClosure.uexp"));
	TArray<FGitChangedAssetEntry> ConflictClosureEntries;
	TArray<FGitChangedAssetArtifact> ConflictClosureArtifacts;
	Error.Reset();
	if (!TestTrue(TEXT("Primary plus conflicted sidecar status parses"), FGitChangedAssetsStatus::ParsePorcelainV2WithArtifacts(ConflictClosureOutput, FPaths::ProjectDir(), ConflictClosureEntries, ConflictClosureArtifacts, Error))) return false;
	const FGitChangedAssetEntry* ConflictClosureMap = FindByPath(ConflictClosureEntries, TEXT("Content/ConflictClosure.umap"));
	if (!TestNotNull(TEXT("Primary plus conflicted sidecar keeps the map row"), ConflictClosureMap)) return false;
	TestEqual(TEXT("Conflicted sidecar makes the primary map conflicted"), ConflictClosureMap->State, EGitChangedAssetState::Conflicted);
	TestFalse(TEXT("Conflicted sidecar blocks map revert eligibility"), ConflictClosureMap->bBaseRevertEligible);
	TArray<uint8> CrossKindRenameOutput;
	AppendNulUtf8(CrossKindRenameOutput, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/CrossKind.umap"));
	AppendNulUtf8(CrossKindRenameOutput, TEXT("Content/CrossKind.uasset"));
	TArray<FGitChangedAssetEntry> CrossKindEntries;
	Error.Reset();
	if (!TestTrue(TEXT("Cross-kind map rename status parses"), FGitChangedAssetsStatus::ParsePorcelainV2(CrossKindRenameOutput, FPaths::ProjectDir(), CrossKindEntries, Error))) return false;
	const FGitChangedAssetEntry* CrossKindEntry = FindByPath(CrossKindEntries, TEXT("Content/CrossKind.umap"));
	if (!TestNotNull(TEXT("Cross-kind rename retains current map row for diagnosis"), CrossKindEntry)) return false;
	TestFalse(TEXT("Cross-kind .umap/.uasset rename is not revert eligible"), CrossKindEntry->bBaseRevertEligible);
	TArray<uint8> CrossStemRenameOutput;
	AppendNulUtf8(CrossStemRenameOutput, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/CrossStem.umap"));
	AppendNulUtf8(CrossStemRenameOutput, TEXT("Content/OldStem.umap"));
	AppendNulUtf8(CrossStemRenameOutput, TEXT("2 R. N... 100644 100644 100644 1111111 2222222 R100 Content/CrossStem.uexp"));
	AppendNulUtf8(CrossStemRenameOutput, TEXT("Content/OtherStem.uexp"));
	TArray<FGitChangedAssetEntry> CrossStemEntries;
	TArray<FGitChangedAssetArtifact> CrossStemArtifacts;
	Error.Reset();
	if (!TestTrue(TEXT("Cross-stem primary and sidecar rename status parses"), FGitChangedAssetsStatus::ParsePorcelainV2WithArtifacts(
		CrossStemRenameOutput, FPaths::ProjectDir(), CrossStemEntries, CrossStemArtifacts, Error))) return false;
	const FGitChangedAssetEntry* CrossStemEntry = FindByPath(CrossStemEntries, TEXT("Content/CrossStem.umap"));
	if (!TestNotNull(TEXT("Cross-stem rename retains current map row for diagnosis"), CrossStemEntry)) return false;
	TestTrue(TEXT("Cross-stem sidecar rename remains in the artifact closure"), CrossStemArtifacts.ContainsByPredicate([](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == TEXT("Content/CrossStem.uexp") && Artifact.Kind == EGitChangedAssetArtifactKind::Sidecar;
	}));
	TestFalse(TEXT("Cross-stem package rename is not revert eligible"), CrossStemEntry->bBaseRevertEligible);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsTrackedReplacementAutomationTest, "UEGitPlugin.ChangedAssets.TrackedReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsRealMapSnapshotAutomationTest, "UEGitPlugin.ChangedAssets.RealMapSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsRealMapSnapshotAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetsStatusAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}
	FScopedMapFixture Map(*this, Fixture);
	const FString SidecarRelativePath = TEXT("Content/Maps/StatusFixture.uexp");
	const FString UnchangedSidecarRelativePath = TEXT("Content/Maps/StatusFixture.ubulk");
	if (!Map.Create()
		|| !Fixture.WriteFile(SidecarRelativePath, TEXT("head sidecar bytes\n"))
		|| !Fixture.WriteFile(UnchangedSidecarRelativePath, TEXT("unchanged sidecar bytes\n"))
		|| !Fixture.CommitAll(TEXT("Initial real umap fixture"))
		|| !Map.AddRevisionActorAndSave()
		|| !Fixture.WriteFile(SidecarRelativePath, TEXT("dirty sidecar bytes\n"))
		)
	{
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("Changed Assets snapshot accepts a real saved .umap"),
		FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString RelativeMapPath = TEXT("Content/Maps/StatusFixture.umap");
	const FGitChangedAssetEntry* MapEntry = FindByPath(Snapshot.Entries, RelativeMapPath);
	if (!TestNotNull(TEXT("Real .umap appears in Changed Assets snapshot"), MapEntry))
	{
		return false;
	}
	TestEqual(TEXT("Real .umap is reported as modified"), MapEntry->State, EGitChangedAssetState::Modified);
	TestEqual(TEXT("Real .umap keeps exact repository-relative identity"), MapEntry->RepositoryRelativePath, RelativeMapPath);
	TestEqual(TEXT("Real .umap entry resolves to the saved map filename"), MapEntry->AbsoluteFilename, Map.GetPackageFilename());
	FGitMapWorldClosure WorldClosure;
	Error.Reset();
	if (!TestTrue(TEXT("Loaded real map builds a deterministic world closure"), GitMapPackageSet::BuildMapWorldClosure(*MapEntry, WorldClosure, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("World closure identifies the real map package"), WorldClosure.MapPackageName, Map.GetPackageName());
	const FGitChangedAssetArtifact* SidecarArtifact = Snapshot.ArtifactEntries.FindByPredicate([&SidecarRelativePath](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == SidecarRelativePath;
	});
	if (!TestNotNull(TEXT("Changed map sidecar is retained in the artifact snapshot"), SidecarArtifact))
	{
		return false;
	}
	TestEqual(TEXT("Changed map sidecar is classified as a sidecar artifact"), SidecarArtifact->Kind, EGitChangedAssetArtifactKind::Sidecar);
	TestEqual(TEXT("Changed map sidecar state is aggregated"), SidecarArtifact->State, EGitChangedAssetState::Modified);
	FGitChangedAssetMutationSet MapOnlySet;
	Error.Reset();
	if (!TestTrue(TEXT("Map-only selection builds an atomic mutation set"),
		GitMapPackageSet::BuildSelectionMutationSet(Snapshot, { *MapEntry }, EGitChangedAssetOperationMode::ChangedAssetsRevert, MapOnlySet, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Map-only set keeps one selected primary package"), MapOnlySet.SelectedEntries.Num(), 1);
	TestTrue(TEXT("Map-only set includes the selected map artifact"), MapOnlySet.Artifacts.ContainsByPredicate([&RelativeMapPath](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == RelativeMapPath && Artifact.Kind == EGitChangedAssetArtifactKind::PrimaryPackage;
	}));
	TestTrue(TEXT("Map-only set includes the selected map sidecar"), MapOnlySet.Artifacts.ContainsByPredicate([&SidecarRelativePath](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == SidecarRelativePath && Artifact.Kind == EGitChangedAssetArtifactKind::Sidecar;
	}));
	TestTrue(TEXT("Map-only set includes an unchanged sidecar baseline"), MapOnlySet.Artifacts.ContainsByPredicate([&UnchangedSidecarRelativePath](const FGitChangedAssetArtifact& Artifact)
	{
		return Artifact.RepositoryRelativePath == UnchangedSidecarRelativePath && Artifact.Kind == EGitChangedAssetArtifactKind::Sidecar && Artifact.bKnownUnchanged;
	}));
	return true;
}

#endif
