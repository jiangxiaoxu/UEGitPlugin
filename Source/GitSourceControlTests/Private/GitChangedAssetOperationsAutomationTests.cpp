// Copyright (c) 2026

#include "GitChangedAssetOperations.h"
#include "GitChangedAssetsStatus.h"
#include "GitMapPackageSet.h"
#include "GitSourceControlUtils.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
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

	class FFixture final
	{
	public:
		explicit FFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FFixture()
		{
			const FString Parent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitChangedAssetOperationsTests"));
			if (!Root.IsEmpty() && (FPaths::IsSamePath(Root, Parent) || FPaths::IsUnderDirectory(Root, Parent)) && IFileManager::Get().DirectoryExists(*Root))
			{
				IFileManager::Get().DeleteDirectory(*Root, false, true);
			}
		}

		bool Initialize()
		{
			GitBinary = GitSourceControlUtils::FindGitBinaryPath();
			Root = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetOperationsTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			if (GitBinary.IsEmpty() || !IFileManager::Get().MakeDirectory(*Root, true))
			{
				Test.AddError(TEXT("Could not create the Changed Assets Git fixture."));
				return false;
			}
			return RunGit(TEXT("init")) && RunGit(TEXT("config user.name \"GitChangedAssetsTests\"")) && RunGit(TEXT("config user.email \"git-changed-assets@example.invalid\""));
		}

		bool WriteFile(const FString& RelativePath, const FString& Contents) const
		{
			const FString Filename = AbsoluteFilename(RelativePath);
			return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true) && FFileHelper::SaveStringToFile(Contents, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool ReadFile(const FString& RelativePath, FString& OutContents) const { return FFileHelper::LoadFileToString(OutContents, *AbsoluteFilename(RelativePath)); }
		bool CommitAll(const FString& Message) const { return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(Message))); }
		FString AbsoluteFilename(const FString& RelativePath) const { return FPaths::Combine(Root, RelativePath); }
		const FString& GetRoot() const { return Root; }
		const FString& GetGitBinary() const { return GitBinary; }

		bool RunGit(const FString& Arguments, FString& OutOutput) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString StandardError;
			const FString FullArguments = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *Arguments);
			FPlatformProcess::ExecProcess(*GitBinary, *FullArguments, &ReturnCode, &OutOutput, &StandardError);
			if (ReturnCode == 0) return true;
			Test.AddError(FString::Printf(TEXT("Git fixture command failed: git %s\n%s"), *FullArguments, *StandardError));
			return false;
		}

		bool RunGit(const FString& Arguments) const
		{
			FString Output;
			return RunGit(Arguments, Output);
		}

	private:
		FAutomationTestBase& Test;
		FString GitBinary;
		FString Root;
	};

	class FScopedCurrentEditorWorldOverride final
	{
	public:
		explicit FScopedCurrentEditorWorldOverride(UWorld* InWorld) { GitChangedAssetOperations::FGitChangedAssetRevertLifecycle::SetCurrentEditorWorldForTesting(InWorld); }
		~FScopedCurrentEditorWorldOverride() { GitChangedAssetOperations::FGitChangedAssetRevertLifecycle::SetCurrentEditorWorldForTesting(nullptr); }
	};

	GitChangedAssetOperations::FGitChangedAssetRevertCallbacks MakeCallbacks()
	{
		GitChangedAssetOperations::FGitChangedAssetRevertCallbacks Result;
		Result.Confirm = [](const TArray<FGitChangedAssetEntry>&, FString&) { return true; };
		Result.PrepareForMutation = [](const TArray<FGitChangedAssetEntry>&, FString&) { return true; };
		Result.BeginMutation = [](const TArray<FGitChangedAssetEntry>&, FString&) { return true; };
		Result.FinalizeEditor = [](const TArray<FGitChangedAssetEntry>&, const TArray<FString>&, GitChangedAssetOperations::EGitChangedAssetMutationOutcome, FString&) { return true; };
		return Result;
	}

	bool CaptureEntries(FAutomationTestBase& Test, const FFixture& Fixture, FString& OutPinnedHead, TArray<FGitChangedAssetEntry>& OutEntries)
	{
		FGitChangedAssetSnapshot Snapshot;
		FString Error;
		if (!FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error))
		{
			Test.AddError(Error);
			return false;
		}
		OutPinnedHead = Snapshot.PinnedHead;
		OutEntries = MoveTemp(Snapshot.Entries);
		for (FGitChangedAssetEntry& Entry : OutEntries)
		{
			Entry.bMetadataResolved = true;
			Entry.bBaseRevertEligible = true;
			Entry.bCanRevert = true;
			Entry.PackageKind = EGitChangedAssetPackageKind::Regular;
		}
		return true;
	}

	bool WritePackageFixture(const FFixture& Fixture, const FString& RelativePath, uint32 Marker)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(sizeof(uint32) * 2);
		const uint32 PackageTag = PACKAGE_FILE_TAG;
		FMemory::Memcpy(Bytes.GetData(), &PackageTag, sizeof(PackageTag));
		FMemory::Memcpy(Bytes.GetData() + sizeof(PackageTag), &Marker, sizeof(Marker));
		const FString Filename = Fixture.AbsoluteFilename(RelativePath);
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true) && FFileHelper::SaveArrayToFile(Bytes, *Filename);
	}

	FGitChangedAssetEntry MakeOfpaEntry(const FString& RelativePath, const FString& PackageName, const FString& OwnerPackageName)
	{
		FGitChangedAssetEntry Entry;
		Entry.RepositoryRelativePath = RelativePath;
		Entry.AbsoluteFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		Entry.PackageName = PackageName;
		Entry.OwnerLevel = OwnerPackageName;
		Entry.State = EGitChangedAssetState::Modified;
		Entry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
		Entry.bMetadataResolved = true;
		Entry.bOwnerLevelResolved = true;
		Entry.bBaseRevertEligible = true;
		Entry.bCanRevert = true;
		Entry.IndexStatus = TEXT('M');
		Entry.WorktreeStatus = TEXT('M');
		return Entry;
	}

	bool BuildHistoricalRequest(FAutomationTestBase& Test, const FFixture& Fixture, const FGitChangedAssetSnapshot& Snapshot, const FGitChangedAssetEntry& Entry,
		const FString& Revision, const FString& PackageName, bool bIsMap, GitChangedAssetOperations::FGitChangedAssetRevisionRestoreRequest& OutRequest)
	{
		FGitChangedAssetMutationSet MutationSet;
		FString Error;
		if (!GitMapPackageSet::BuildSelectionMutationSet(Snapshot, { Entry }, EGitChangedAssetOperationMode::HistoricalRestore, MutationSet, Error))
		{
			Test.AddError(Error);
			return false;
		}
		FGitChangedPrimaryPackageTarget Target;
		Target.PackageName = PackageName;
		Target.RepositoryRelativePath = Entry.RepositoryRelativePath;
		Target.AbsoluteFilename = Entry.AbsoluteFilename;
		Target.bIsMap = bIsMap;
		FGitPackageRevisionArtifactSet Artifacts;
		if (!GitMapPackageSet::BuildPackageRevisionArtifactSet(Fixture.GetGitBinary(), Fixture.GetRoot(), Revision, Target, Artifacts, Error))
		{
			Test.AddError(Error);
			return false;
		}
		OutRequest = { MoveTemp(MutationSet), MoveTemp(Artifacts) };
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetMutationAtomicityAutomationTest, "UEGitPlugin.ChangedAssets.MutationAtomicity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetMutationAtomicityAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;

	{
		FFixture Fixture(*this);
		if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Selected.uasset"), TEXT("head selected\n")) || !Fixture.WriteFile(TEXT("Content/Unselected.uasset"), TEXT("head unselected\n")) || !Fixture.CommitAll(TEXT("Selection baseline")) || !Fixture.WriteFile(TEXT("Content/Selected.uasset"), TEXT("dirty selected\n")) || !Fixture.WriteFile(TEXT("Content/Unselected.uasset"), TEXT("dirty unselected\n"))) return false;
		FString Head;
		TArray<FGitChangedAssetEntry> Entries;
		if (!CaptureEntries(*this, Fixture, Head, Entries)) return false;
		Entries.RemoveAll([](const FGitChangedAssetEntry& Entry) { return Entry.RepositoryRelativePath == TEXT("Content/Unselected.uasset"); });
		FGitChangedAssetRevertResult Result;
		if (!TestTrue(TEXT("Exact selected asset reverts"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(Head, Entries, MakeCallbacks(), Result))) return false;
		FString Contents;
		if (!TestTrue(TEXT("Selected asset is readable"), Fixture.ReadFile(TEXT("Content/Selected.uasset"), Contents)) || !TestEqual(TEXT("Selected asset returns to HEAD"), Contents, FString(TEXT("head selected\n"))) || !TestTrue(TEXT("Unselected asset is readable"), Fixture.ReadFile(TEXT("Content/Unselected.uasset"), Contents)) || !TestEqual(TEXT("Exact selection leaves unselected asset dirty"), Contents, FString(TEXT("dirty unselected\n")))) return false;

		TArray<FGitChangedAssetEntry> CancelledEntries;
		if (!CaptureEntries(*this, Fixture, Head, CancelledEntries)) return false;
		FGitChangedAssetRevertCallbacks CancelledCallbacks = MakeCallbacks();
		CancelledCallbacks.IsCancellationRequested = []() { return true; };
		Result = FGitChangedAssetRevertResult();
		TestFalse(TEXT("Cancellation before transaction commit point aborts mutation"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(Head, CancelledEntries, CancelledCallbacks, Result));
		if (!TestTrue(TEXT("Cancellation is explicit"), Result.bCancelled) || !TestTrue(TEXT("Cancelled worktree is readable"), Fixture.ReadFile(TEXT("Content/Unselected.uasset"), Contents))) return false;
		TestEqual(TEXT("Cancellation preserves worktree bytes"), Contents, FString(TEXT("dirty unselected\n")));
	}

	{
		FFixture Fixture(*this);
		if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Tracked.uasset"), TEXT("head\n")) || !Fixture.CommitAll(TEXT("Rollback baseline")) || !Fixture.WriteFile(TEXT("Content/Added.uasset"), TEXT("staged\n")) || !Fixture.RunGit(TEXT("add -- Content/Added.uasset"))) return false;
		FString IndexBefore;
		if (!Fixture.RunGit(TEXT("ls-files --stage -- Content/Added.uasset"), IndexBefore)) return false;
		FString Head;
		TArray<FGitChangedAssetEntry> Entries;
		if (!CaptureEntries(*this, Fixture, Head, Entries)) return false;
		FGitChangedAssetRevertCallbacks Callbacks = MakeCallbacks();
		Callbacks.AllowFilesystemMutationForTesting = []() { return false; };
		FGitChangedAssetRevertResult Result;
		TestFalse(TEXT("Injected filesystem failure rolls back Git mutation"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(Head, Entries, Callbacks, Result));
		FString IndexAfter;
		FString Contents;
		if (!TestTrue(TEXT("Rollback retains index"), Fixture.RunGit(TEXT("ls-files --stage -- Content/Added.uasset"), IndexAfter)) || !TestEqual(TEXT("Rollback restores exact index"), IndexAfter, IndexBefore) || !TestTrue(TEXT("Rollback retains worktree"), Fixture.ReadFile(TEXT("Content/Added.uasset"), Contents))) return false;
		TestEqual(TEXT("Rollback restores exact worktree bytes"), Contents, FString(TEXT("staged\n")));
	}

	FFixture Fixture(*this);
	const FString LfsPointer = TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nsize 17\n");
	if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Plain.uasset"), TEXT("head plain\n")) || !Fixture.WriteFile(TEXT("Content/Lfs.uasset"), LfsPointer) || !Fixture.CommitAll(TEXT("LFS preflight baseline")) || !Fixture.WriteFile(TEXT("Content/Plain.uasset"), TEXT("dirty plain\n")) || !Fixture.WriteFile(TEXT("Content/Lfs.uasset"), TEXT("dirty LFS\n"))) return false;
	FString Head;
	TArray<FGitChangedAssetEntry> Entries;
	if (!CaptureEntries(*this, Fixture, Head, Entries)) return false;
	FGitChangedAssetRevertResult Result;
	TestFalse(TEXT("Missing LFS object rejects mixed selection before partial mutation"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(Head, Entries, MakeCallbacks(), Result));
	FString Contents;
	if (!TestTrue(TEXT("Plain asset remains after LFS preflight"), Fixture.ReadFile(TEXT("Content/Plain.uasset"), Contents)) || !TestEqual(TEXT("Plain asset has no partial restore"), Contents, FString(TEXT("dirty plain\n"))) || !TestTrue(TEXT("LFS asset remains after preflight"), Fixture.ReadFile(TEXT("Content/Lfs.uasset"), Contents))) return false;
	return TestEqual(TEXT("LFS asset has no partial restore"), Contents, FString(TEXT("dirty LFS\n")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetReloadSafetyAutomationTest, "UEGitPlugin.ChangedAssets.ReloadSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetReloadSafetyAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	{
		FFixture Fixture(*this);
		if (!Fixture.Initialize() || !Fixture.WriteFile(TEXT("Content/Reload.uasset"), TEXT("head\n")) || !Fixture.CommitAll(TEXT("Reload baseline")) || !Fixture.WriteFile(TEXT("Content/Reload.uasset"), TEXT("dirty\n"))) return false;
		FString Head;
		TArray<FGitChangedAssetEntry> Entries;
		if (!CaptureEntries(*this, Fixture, Head, Entries)) return false;
		FGitChangedAssetRevertCallbacks Callbacks = MakeCallbacks();
		Callbacks.FinalizeEditor = [](const TArray<FGitChangedAssetEntry>&, const TArray<FString>&, EGitChangedAssetMutationOutcome, FString& OutError) { OutError = TEXT("Intentional reload failure."); return false; };
		FGitChangedAssetRevertResult Result;
		if (!TestTrue(TEXT("Disk transaction succeeds despite reload failure"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(Head, Entries, Callbacks, Result)) || !TestTrue(TEXT("Disk success is retained"), Result.bDiskMutationSucceeded && Result.bSucceeded) || !TestFalse(TEXT("Reload failure is separate"), Result.bReloadSucceeded)) return false;
	}

	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const CurrentPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsCurrent_%s"), *Id));
	UPackage* const OwnerPackage = CreatePackage(*FString::Printf(TEXT("/Game/GitChangedAssetsOwner_%s"), *Id));
	if (!TestNotNull(TEXT("Creates current world package"), CurrentPackage) || !TestNotNull(TEXT("Creates owner world package"), OwnerPackage)) return false;
	UWorld* const CurrentWorld = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("Current_%s"), *Id)), CurrentPackage);
	UWorld* const OwnerWorld = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("Owner_%s"), *Id)), OwnerPackage);
	if (!TestNotNull(TEXT("Creates current Editor world"), CurrentWorld) || !TestNotNull(TEXT("Creates direct non-current owner world"), OwnerWorld)) return false;
	const TArray<FString> ExternalRoots = ULevel::GetExternalActorsPaths(OwnerPackage->GetName());
	if (!TestTrue(TEXT("Derives external actor root"), !ExternalRoots.IsEmpty())) return false;
	UPackage* const SelectedPackage = CreatePackage(*(ExternalRoots[0] + TEXT("/A/B/Selected")));
	UPackage* const SiblingPackage = CreatePackage(*(ExternalRoots[0] + TEXT("/A/B/Sibling")));
	AActor* const Selected = SelectedPackage != nullptr ? NewObject<AActor>(OwnerWorld->PersistentLevel, FName(*FString::Printf(TEXT("Selected_%s"), *Id)), RF_Public | RF_Standalone) : nullptr;
	AActor* const Sibling = SiblingPackage != nullptr ? NewObject<AActor>(OwnerWorld->PersistentLevel, FName(*FString::Printf(TEXT("Sibling_%s"), *Id)), RF_Public | RF_Standalone) : nullptr;
	if (!TestNotNull(TEXT("Creates selected OFPA package"), SelectedPackage) || !TestNotNull(TEXT("Creates sibling OFPA package"), SiblingPackage) || !TestNotNull(TEXT("Creates selected OFPA actor"), Selected) || !TestNotNull(TEXT("Creates sibling OFPA actor"), Sibling)) return false;
	OwnerWorld->PersistentLevel->Actors.Add(Selected);
	OwnerWorld->PersistentLevel->Actors.Add(Sibling);
	Selected->SetPackageExternal(true, false, SelectedPackage);
	Sibling->SetPackageExternal(true, false, SiblingPackage);
	SelectedPackage->SetDirtyFlag(false);
	OwnerPackage->SetDirtyFlag(false);
	FScopedCurrentEditorWorldOverride CurrentWorldOverride(CurrentWorld);
	FGitChangedAssetRevertPreview Preview;
	FString Error;
	const FGitChangedAssetEntry SelectedEntry = MakeOfpaEntry(TEXT("Content/__ExternalActors__/Selected.uasset"), SelectedPackage->GetName(), OwnerPackage->GetName());
	SiblingPackage->SetDirtyFlag(true);
	TestFalse(TEXT("Dirty unselected OFPA sibling blocks reload"), FGitChangedAssetRevertLifecycle::BuildPreview({ SelectedEntry }, Preview, Error));
	SiblingPackage->SetDirtyFlag(false);
	Error.Reset();
	if (!TestTrue(TEXT("Clean direct owner builds lifecycle"), FGitChangedAssetRevertLifecycle::BuildPreview({ SelectedEntry }, Preview, Error))) { AddError(Error); return false; }
	return TestTrue(TEXT("Direct non-current owner is reloaded"), Preview.OwnerMapsToReload.Contains(OwnerPackage->GetName()));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetMapTransactionAutomationTest, "UEGitPlugin.ChangedAssets.MapTransaction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetMapTransactionAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	FFixture Fixture(*this);
	if (!Fixture.Initialize()) return false;
	const FString ContentDirectory = FPaths::Combine(Fixture.GetRoot(), TEXT("Content"));
	if (!TestTrue(TEXT("Creates map content root"), IFileManager::Get().MakeDirectory(*ContentDirectory, true))) return false;
	const FString MountRoot = FString::Printf(TEXT("/GitChangedAssetsMap_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory); };
	const FString MapPackageName = MountRoot + TEXT("Maps/Transaction");
	UPackage* const MapPackage = CreatePackage(*MapPackageName);
	UWorld* const MapWorld = MapPackage != nullptr ? UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("ChangedAssetsMap")), MapPackage) : nullptr;
	if (!TestNotNull(TEXT("Creates real map world"), MapWorld)) return false;
	FSavePackageArgs SaveArgs;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("Saves real map package"), UPackage::SavePackage(MapPackage, MapWorld, *FPackageName::LongPackageNameToFilename(MapPackageName, FPackageName::GetMapPackageExtension()), SaveArgs))) return false;
	MapPackage->SetDirtyFlag(false);
	const FString MapPath = TEXT("Content/Maps/Transaction.umap");
	const FString SidecarPath = TEXT("Content/Maps/Transaction.uexp");
	const FString SelectedPath = TEXT("Content/__ExternalActors__/Selected.uasset");
	const FString UnselectedPath = TEXT("Content/__ExternalActors__/Unselected.uasset");
	if (!Fixture.WriteFile(SidecarPath, TEXT("head sidecar\n")) || !WritePackageFixture(Fixture, SelectedPath, 1) || !WritePackageFixture(Fixture, UnselectedPath, 2) || !Fixture.CommitAll(TEXT("Map transaction baseline"))) return false;
	FString Head;
	if (!Fixture.RunGit(TEXT("rev-parse HEAD"), Head)) return false;
	Head.TrimStartAndEndInline();
	TArray<uint8> HeadSelected;
	if (!FFileHelper::LoadFileToArray(HeadSelected, *Fixture.AbsoluteFilename(SelectedPath))) return false;
	if (!TestNotNull(TEXT("Changes map world"), MapWorld->SpawnActor<AActor>()) || !TestTrue(TEXT("Resaves changed map"), UPackage::SavePackage(MapPackage, MapWorld, *FPackageName::LongPackageNameToFilename(MapPackageName, FPackageName::GetMapPackageExtension()), SaveArgs)) || !Fixture.WriteFile(SidecarPath, TEXT("dirty sidecar\n")) || !WritePackageFixture(Fixture, SelectedPath, 3) || !WritePackageFixture(Fixture, UnselectedPath, 4)) return false;
	TArray<uint8> DirtyUnselected;
	if (!FFileHelper::LoadFileToArray(DirtyUnselected, *Fixture.AbsoluteFilename(UnselectedPath))) return false;
	MapPackage->SetDirtyFlag(false);
	TArray<UPackage*> ToUnload = { MapPackage };
	FText UnloadError;
	if (!TestTrue(TEXT("Unloads map before file transaction"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true))) return false;
	FGitChangedAssetSnapshot Snapshot;
	FString Error;
	if (!FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)) { AddError(Error); return false; }
	const FGitChangedAssetEntry* MapEntry = Snapshot.Entries.FindByPredicate([&MapPath](const FGitChangedAssetEntry& Entry) { return Entry.RepositoryRelativePath == MapPath; });
	const FGitChangedAssetEntry* SelectedEntry = Snapshot.Entries.FindByPredicate([&SelectedPath](const FGitChangedAssetEntry& Entry) { return Entry.RepositoryRelativePath == SelectedPath; });
	if (!TestNotNull(TEXT("Captures map entry"), MapEntry) || !TestNotNull(TEXT("Captures selected OFPA entry"), SelectedEntry)) return false;
	FGitChangedAssetEntry SelectedMap = *MapEntry;
	SelectedMap.bMetadataResolved = SelectedMap.bBaseRevertEligible = SelectedMap.bCanRevert = true;
	SelectedMap.PackageKind = EGitChangedAssetPackageKind::Regular;
	FGitChangedAssetEntry SelectedOfpa = *SelectedEntry;
	SelectedOfpa.bMetadataResolved = SelectedOfpa.bBaseRevertEligible = SelectedOfpa.bCanRevert = true;
	SelectedOfpa.PackageKind = EGitChangedAssetPackageKind::Regular;
	FGitChangedAssetMutationSet MapOnlySet;
	if (!TestTrue(TEXT("Builds map sidecar closure"), GitMapPackageSet::BuildSelectionMutationSet(Snapshot, { SelectedMap }, EGitChangedAssetOperationMode::ChangedAssetsRevert, MapOnlySet, Error))) { AddError(Error); return false; }
	TestTrue(TEXT("Map closure contains same-stem sidecar"), MapOnlySet.Artifacts.ContainsByPredicate([&SidecarPath](const FGitChangedAssetArtifact& Artifact) { return Artifact.RepositoryRelativePath == SidecarPath; }));
	FGitChangedAssetRevertCallbacks Rollback = MakeCallbacks();
	Rollback.AllowFilesystemMutationForTesting = []() { return false; };
	FGitChangedAssetRevertResult Result;
	TestFalse(TEXT("Map sidecar rollback is atomic"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(MapOnlySet, Rollback, Result));
	FString Sidecar;
	TArray<uint8> CurrentUnselected;
	if (!TestTrue(TEXT("Rollback retains sidecar"), Fixture.ReadFile(SidecarPath, Sidecar)) || !TestEqual(TEXT("Rollback retains dirty sidecar bytes"), Sidecar, FString(TEXT("dirty sidecar\n"))) || !TestTrue(TEXT("Rollback retains unselected OFPA"), FFileHelper::LoadFileToArray(CurrentUnselected, *Fixture.AbsoluteFilename(UnselectedPath))) || !TestTrue(TEXT("Map-only rollback leaves unselected OFPA untouched"), CurrentUnselected == DirtyUnselected)) return false;
	FGitChangedAssetMutationSet UnionSet;
	Error.Reset();
	if (!TestTrue(TEXT("Builds map plus selected OFPA closure"), GitMapPackageSet::BuildSelectionMutationSet(Snapshot, { SelectedMap, SelectedOfpa }, EGitChangedAssetOperationMode::ChangedAssetsRevert, UnionSet, Error))) { AddError(Error); return false; }
	if (!TestTrue(TEXT("Map and selected OFPA revert atomically"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RevertToHead(UnionSet, MakeCallbacks(), Result))) return false;
	TArray<uint8> SelectedBytes;
	if (!TestTrue(TEXT("Selected OFPA returns to HEAD"), FFileHelper::LoadFileToArray(SelectedBytes, *Fixture.AbsoluteFilename(SelectedPath))) || !TestTrue(TEXT("Selected OFPA bytes match HEAD"), SelectedBytes == HeadSelected) || !TestTrue(TEXT("Unselected OFPA remains after union transaction"), FFileHelper::LoadFileToArray(CurrentUnselected, *Fixture.AbsoluteFilename(UnselectedPath))) || !TestTrue(TEXT("Union transaction leaves unselected OFPA untouched"), CurrentUnselected == DirtyUnselected)) return false;

	UPackage* const OwnerPackage = CreatePackage(*(MountRoot + TEXT("Maps/OwnerLifecycle")));
	UWorld* const OwnerWorld = OwnerPackage != nullptr ? UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("ChangedAssetsOwnerLifecycle")), OwnerPackage) : nullptr;
	if (!TestNotNull(TEXT("Creates loaded map owner for OFPA lifecycle"), OwnerWorld)) return false;
	const TArray<FString> ExternalRoots = ULevel::GetExternalActorsPaths(OwnerPackage->GetName());
	if (!TestTrue(TEXT("Derives loaded owner external actor root"), !ExternalRoots.IsEmpty())) return false;
	UPackage* const ActorPackage = CreatePackage(*(ExternalRoots[0] + TEXT("/A/B/Actor")));
	AActor* const Actor = ActorPackage != nullptr ? NewObject<AActor>(OwnerWorld->PersistentLevel, FName(TEXT("ChangedAssetsLifecycleActor")), RF_Public | RF_Standalone) : nullptr;
	if (!TestNotNull(TEXT("Creates loaded external actor"), Actor)) return false;
	OwnerWorld->PersistentLevel->Actors.Add(Actor);
	Actor->SetPackageExternal(true, false, ActorPackage);
	ActorPackage->SetDirtyFlag(false);
	OwnerPackage->SetDirtyFlag(false);
	FScopedCurrentEditorWorldOverride OwnerWorldOverride(OwnerWorld);
	FGitChangedAssetRevertPreview OwnerPreview;
	Error.Reset();
	if (!TestTrue(TEXT("Selected OFPA resolves loaded owner lifecycle"), FGitChangedAssetRevertLifecycle::BuildPreview({ MakeOfpaEntry(TEXT("Content/__ExternalActors__/Lifecycle.uasset"), ActorPackage->GetName(), OwnerPackage->GetName()) }, OwnerPreview, Error))) { AddError(Error); return false; }
	return TestTrue(TEXT("Map transaction lifecycle includes selected OFPA owner"), OwnerPreview.OwnerMapsToReload.Contains(OwnerPackage->GetName()));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetHistoricalRestoreTransactionAutomationTest, "UEGitPlugin.ChangedAssets.HistoricalRestoreTransaction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetHistoricalRestoreTransactionAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetOperations;
	using namespace GitChangedAssetOperationsAutomationTestsPrivate;
	{
		FFixture Fixture(*this);
		const FString MapPath = TEXT("Content/Maps/Untracked.umap");
		if (!Fixture.Initialize() || !WritePackageFixture(Fixture, MapPath, 1) || !Fixture.CommitAll(TEXT("Untracked historical baseline"))) return false;
		FString Revision;
		if (!Fixture.RunGit(TEXT("rev-parse HEAD"), Revision) || !Fixture.RunGit(FString::Printf(TEXT("rm -- %s"), *QuoteGitArgument(MapPath))) || !Fixture.CommitAll(TEXT("Remove current map")) || !WritePackageFixture(Fixture, MapPath, 2)) return false;
		Revision.TrimStartAndEndInline();
		FGitChangedAssetSnapshot Snapshot;
		FString Error;
		if (!FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)) { AddError(Error); return false; }
		const FGitChangedAssetEntry* Entry = Snapshot.Entries.FindByPredicate([&MapPath](const FGitChangedAssetEntry& Candidate) { return Candidate.RepositoryRelativePath == MapPath; });
		if (!TestNotNull(TEXT("Captures untracked replacement"), Entry)) return false;
		FGitChangedAssetRevisionRestoreRequest Request;
		if (!BuildHistoricalRequest(*this, Fixture, Snapshot, *Entry, Revision, TEXT("/Game/Maps/Untracked"), true, Request)) return false;
		FGitChangedAssetRevertResult Result;
		if (!TestTrue(TEXT("Historical restore replaces untracked map"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RestorePackageRevision(Request, MakeCallbacks(), Result))) return false;
	}

	{
		FFixture Fixture(*this);
		const FString MapPath = TEXT("Content/Maps/Deleted.umap");
		const FString SidecarPath = TEXT("Content/Maps/Deleted.uexp");
		if (!Fixture.Initialize() || !WritePackageFixture(Fixture, MapPath, 11) || !Fixture.WriteFile(SidecarPath, TEXT("head sidecar\n")) || !Fixture.CommitAll(TEXT("Deleted historical baseline"))) return false;
		FString Revision;
		if (!Fixture.RunGit(TEXT("rev-parse HEAD"), Revision) || !Fixture.RunGit(FString::Printf(TEXT("rm -- %s %s"), *QuoteGitArgument(MapPath), *QuoteGitArgument(SidecarPath)))) return false;
		Revision.TrimStartAndEndInline();
		FGitChangedAssetSnapshot Snapshot;
		FString Error;
		if (!FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)) { AddError(Error); return false; }
		const FGitChangedAssetEntry* Entry = Snapshot.Entries.FindByPredicate([&MapPath](const FGitChangedAssetEntry& Candidate) { return Candidate.RepositoryRelativePath == MapPath; });
		if (!TestNotNull(TEXT("Captures deleted tracked map"), Entry)) return false;
		FGitChangedAssetRevisionRestoreRequest Request;
		if (!BuildHistoricalRequest(*this, Fixture, Snapshot, *Entry, Revision, TEXT("/Game/Maps/Deleted"), true, Request)) return false;
		FGitChangedAssetRevertResult Result;
		if (!TestTrue(TEXT("Historical restore restores deleted map and sidecar"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RestorePackageRevision(Request, MakeCallbacks(), Result))) return false;
		if (!TestTrue(TEXT("Deleted map sidecar is restored"), FPaths::FileExists(Fixture.AbsoluteFilename(SidecarPath)))) return false;
	}

	{
		FFixture Fixture(*this);
		const FString MapPath = TEXT("Content/Maps/Conflict.umap");
		if (!Fixture.Initialize() || !WritePackageFixture(Fixture, MapPath, 21) || !Fixture.CommitAll(TEXT("Conflict base"))) return false;
		FString Base;
		if (!Fixture.RunGit(TEXT("rev-parse HEAD"), Base)) return false;
		Base.TrimStartAndEndInline();
		if (!WritePackageFixture(Fixture, MapPath, 22) || !Fixture.CommitAll(TEXT("Historical revision"))) return false;
		FString Revision;
		if (!Fixture.RunGit(TEXT("rev-parse HEAD"), Revision)) return false;
		Revision.TrimStartAndEndInline();
		if (!Fixture.RunGit(FString::Printf(TEXT("checkout -b conflict-side %s"), *QuoteGitArgument(Base))) || !WritePackageFixture(Fixture, MapPath, 23) || !Fixture.CommitAll(TEXT("Conflict current revision"))) return false;
		int32 MergeCode = INDEX_NONE;
		FString MergeOutput;
		FString MergeError;
		FPlatformProcess::ExecProcess(*Fixture.GetGitBinary(), *FString::Printf(TEXT("-C %s --no-optional-locks merge --no-commit %s"), *QuoteGitArgument(Fixture.GetRoot()), *QuoteGitArgument(Revision)), &MergeCode, &MergeOutput, &MergeError);
		if (!TestTrue(TEXT("Creates real Git conflict"), MergeCode != 0)) return false;
		FGitChangedAssetSnapshot Snapshot;
		FString Error;
		if (!FGitChangedAssetsStatus::CaptureSnapshot(Fixture.GetGitBinary(), Fixture.GetRoot(), 1, Snapshot, Error)) { AddError(Error); return false; }
		const FGitChangedAssetEntry* Entry = Snapshot.Entries.FindByPredicate([&MapPath](const FGitChangedAssetEntry& Candidate) { return Candidate.RepositoryRelativePath == MapPath; });
		if (!TestNotNull(TEXT("Captures conflicted map"), Entry)) return false;
		FGitChangedAssetRevisionRestoreRequest Request;
		if (!BuildHistoricalRequest(*this, Fixture, Snapshot, *Entry, Revision, TEXT("/Game/Maps/Conflict"), true, Request)) return false;
		FGitChangedAssetRevertResult Result;
		if (!TestTrue(TEXT("Historical restore clears real merge conflict"), FGitChangedAssetOperations(Fixture.GetGitBinary(), Fixture.GetRoot()).RestorePackageRevision(Request, MakeCallbacks(), Result))) return false;
		FString Unmerged;
		if (!Fixture.RunGit(FString::Printf(TEXT("ls-files -u -- %s"), *QuoteGitArgument(MapPath)), Unmerged)) return false;
		Unmerged.TrimStartAndEndInline();
		if (!TestTrue(TEXT("Historical restore clears all index conflict stages"), Unmerged.IsEmpty())) return false;
	}

	{
		const EGitChangedAssetState States[] = { EGitChangedAssetState::Modified, EGitChangedAssetState::Added, EGitChangedAssetState::Untracked, EGitChangedAssetState::Renamed, EGitChangedAssetState::Conflicted };
		for (const EGitChangedAssetState State : States)
		{
			FGitChangedAssetEntry Entry;
			Entry.RepositoryRelativePath = TEXT("Content/Policy.uasset");
			Entry.AbsoluteFilename = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetOperationsTests/Content/Policy.uasset"));
			Entry.State = State;
			Entry.bBaseRevertEligible = true;
			Entry.bCanRevert = true;
			if (State == EGitChangedAssetState::Renamed)
			{
				Entry.RenameFromRepositoryRelativePath = TEXT("Content/PolicyOld.uasset");
				Entry.RenameFromAbsoluteFilename = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitChangedAssetOperationsTests/Content/PolicyOld.uasset"));
			}
			FString Error;
			const bool bDiscardAccepted = FGitChangedAssetOperations::ValidateEntries({ Entry }, EGitChangedAssetOperationMode::DiscardTracked, Error);
			if (State == EGitChangedAssetState::Modified ? !TestTrue(TEXT("Discard accepts tracked modification"), bDiscardAccepted) : !TestFalse(TEXT("Discard rejects non-modified topology"), bDiscardAccepted)) return false;
			Error.Reset();
			if (!TestTrue(TEXT("Historical restore supports current topology"), FGitChangedAssetOperations::ValidateEntries({ Entry }, EGitChangedAssetOperationMode::HistoricalRestore, Error))) return false;
		}
	}

	{
		FFixture Fixture(*this);
		const FString OldMap = TEXT("Content/Maps/Old.umap");
		const FString OldSidecar = TEXT("Content/Maps/Old.uexp");
		const FString NewMap = TEXT("Content/Maps/New.umap");
		const FString NewSidecar = TEXT("Content/Maps/New.uexp");
		if (!Fixture.Initialize() || !WritePackageFixture(Fixture, OldMap, 31) || !Fixture.WriteFile(OldSidecar, TEXT("old sidecar\n")) || !Fixture.CommitAll(TEXT("Exact rename source"))) return false;
		FString SourceRevision;
		if (!Fixture.RunGit(TEXT("rev-parse HEAD"), SourceRevision) || !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(OldMap), *QuoteGitArgument(NewMap))) || !Fixture.RunGit(FString::Printf(TEXT("mv -- %s %s"), *QuoteGitArgument(OldSidecar), *QuoteGitArgument(NewSidecar))) || !Fixture.CommitAll(TEXT("Exact rename destination"))) return false;
		SourceRevision.TrimStartAndEndInline();
		FGitChangedPrimaryPackageTarget Target;
		Target.PackageName = TEXT("/Game/Maps/Old");
		Target.RepositoryRelativePath = OldMap;
		Target.AbsoluteFilename = Fixture.AbsoluteFilename(NewMap);
		Target.bIsMap = true;
		FGitPackageRevisionArtifactSet Artifacts;
		FString Error;
		if (!TestTrue(TEXT("Historical old stem is resolved without local source artifact"), GitMapPackageSet::BuildPackageRevisionArtifactSet(Fixture.GetGitBinary(), Fixture.GetRoot(), SourceRevision, Target, Artifacts, Error))) { AddError(Error); return false; }
		return TestTrue(TEXT("Historical exact rename retains old sidecar stem"), Artifacts.RepositoryRelativePaths.Contains(OldSidecar) && !Artifacts.RepositoryRelativePaths.Contains(NewSidecar));
	}
}

#endif
