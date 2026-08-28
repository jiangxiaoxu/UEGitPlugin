// Copyright (c) 2026

#include "GitChangedAssetsMetadata.h"
#include "GitCatFileBatchReader.h"
#include "GitSourceControlUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/TopLevelAssetPath.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/DataLayer/WorldDataLayersActorDesc.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitChangedAssetsMetadataAutomationTestsPrivate
{
	struct FDataLayerMappingFixture
	{
		FString OwnerLevel;
		FName Identifier;
		FString FriendlyName;
		FAssetData WorldDataLayersAssetData;
	};

	FString GetHeaderFallbackFixtureFilename()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("B_LyraGameInstance.uasset"));
	}

	bool FindPrivateOrDeprecatedDataLayerFixture(FDataLayerMappingFixture& OutFixture)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(AWorldDataLayers::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.bIncludeOnlyOnDiskAssets = true;

		TArray<FAssetData> WorldDataLayersAssets;
		IAssetRegistry::GetChecked().GetAssets(Filter, WorldDataLayersAssets, false);
		for (const FAssetData& AssetData : WorldDataLayersAssets)
		{
			if (!FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(AssetData) ||
				FWorldPartitionActorDescUtils::GetActorNativeClassFromAssetData(AssetData) != AWorldDataLayers::StaticClass())
			{
				continue;
			}
			const TUniquePtr<FWorldPartitionActorDesc> ActorDesc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(AssetData);
			if (!ActorDesc)
			{
				continue;
			}
			const FWorldDataLayersActorDesc& WorldDataLayersDesc = static_cast<const FWorldDataLayersActorDesc&>(*ActorDesc);
			if (!WorldDataLayersDesc.IsValid())
			{
				continue;
			}
			const FString OwnerLevel = WorldDataLayersDesc.GetActorSoftPath().GetAssetPath().GetPackageName().ToString();
			if (!FPackageName::IsValidLongPackageName(OwnerLevel, true))
			{
				continue;
			}
			for (const FDataLayerInstanceDesc& InstanceDesc : WorldDataLayersDesc.GetDataLayerInstances())
			{
				const FName Identifier = InstanceDesc.IsUsingAsset() ? InstanceDesc.GetAssetPath() : InstanceDesc.GetName();
				const FString FriendlyName = InstanceDesc.GetShortName();
				if (Identifier.IsNone() || FriendlyName.IsEmpty() || FriendlyName.Equals(TEXT("Unknown"), ESearchCase::CaseSensitive) ||
					!GitChangedAssetsMetadataTesting::RequiresWorldDataLayersDescriptor(Identifier))
				{
					continue;
				}
				OutFixture.OwnerLevel = OwnerLevel;
				OutFixture.Identifier = Identifier;
				OutFixture.FriendlyName = FriendlyName;
				OutFixture.WorldDataLayersAssetData = AssetData;
				return true;
			}
		}
		return false;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsPackageHeaderMetadataAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.PackageHeaderFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsPackageHeaderMetadataAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString Filename = GitChangedAssetsMetadataAutomationTestsPrivate::GetHeaderFallbackFixtureFilename();
	if (!TestTrue(TEXT("Metadata fixture exists"), FPaths::FileExists(Filename)))
	{
		return false;
	}

	FGitChangedAssetEntry Entry;
	Entry.AbsoluteFilename = Filename;
	Entry.RepositoryRelativePath = TEXT("Content/B_LyraGameInstance.uasset");
	const FAssetData StaleAssetRegistryData(FName(TEXT("/Game/Stale")), FName(TEXT("/Game/Stale")), FName(TEXT("StaleAssetRegistryName")),
		FTopLevelAssetPath(FName(TEXT("/Script/Engine")), FName(TEXT("Object"))));
	GitChangedAssetsMetadataTesting::ApplyAssetData(StaleAssetRegistryData, Entry);
	TestEqual(TEXT("Fixture starts with a stale Asset Registry display value"), Entry.DisplayName, FString(TEXT("StaleAssetRegistryName")));
	FString FailureReason;
	if (!TestTrue(TEXT("Package header fallback succeeds"),
		GitChangedAssetsMetadataTesting::ApplyPackageHeaderMetadata(Filename, Entry, FailureReason)))
	{
		AddError(FailureReason);
		return false;
	}
	TestFalse(TEXT("Header fallback resolves a non-empty name"), Entry.DisplayName.IsEmpty());
	TestFalse(TEXT("Header fallback resolves a non-empty object path"), Entry.ObjectPath.IsEmpty());
	TestFalse(TEXT("Header fallback resolves a non-empty type"), Entry.AssetType.IsEmpty());
	TestFalse(TEXT("Header fallback corrects package name"), Entry.PackageName.IsEmpty());
	TestFalse(TEXT("Current package header replaces stale Asset Registry display metadata"),
		Entry.DisplayName.Equals(TEXT("StaleAssetRegistryName"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("Current display metadata is sourced from the worktree package header"), Entry.MetadataSource,
		EGitChangedAssetMetadataSource::CurrentPackageRegistry);

	FGitChangedAssetEntry MappingEntry;
	MappingEntry.AbsoluteFilename = Filename;
	const FString OriginalAbsoluteFilename = MappingEntry.AbsoluteFilename;
	TestTrue(TEXT("Filename-to-package mapping succeeds"), GitChangedAssetsMetadataTesting::EnsurePackageName(MappingEntry));
	TestEqual(TEXT("Metadata package mapping preserves the absolute Git identity"), MappingEntry.AbsoluteFilename, OriginalAbsoluteFilename);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsLfsHeadMetadataAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.LfsHeadPackageHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsLfsHeadMetadataAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString FixtureFilename = GitChangedAssetsMetadataAutomationTestsPrivate::GetHeaderFallbackFixtureFilename();
	TArray<uint8> PackageBytes;
	if (!TestTrue(TEXT("LFS metadata fixture can be read"), FFileHelper::LoadFileToArray(PackageBytes, *FixtureFilename)))
	{
		return false;
	}

	const FString Oid(TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	const FString RepositoryRoot = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("GitChangedAssetsLfsHeadMetadata"));
	const FString LocalObject = FPaths::Combine(RepositoryRoot, TEXT(".git/lfs/objects/aa/aa"), Oid);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestFalse(TEXT("Git executable is available for local LFS metadata lookup"), GitBinary.IsEmpty()))
	{
		return false;
	}
	IFileManager::Get().DeleteDirectory(*RepositoryRoot, false, true);
	IFileManager::Get().MakeDirectory(*RepositoryRoot, true);
	int32 GitReturnCode = INDEX_NONE;
	FString GitError;
	FPlatformProcess::ExecProcess(*GitBinary, *FString::Printf(TEXT("-C \"%s\" init --quiet"), *RepositoryRoot), &GitReturnCode, nullptr, &GitError);
	if (!TestTrue(TEXT("LFS metadata fixture repository initializes"), GitReturnCode == 0))
	{
		AddError(GitError);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LocalObject), true);
	if (!TestTrue(TEXT("LFS metadata fixture object can be written"), FFileHelper::SaveArrayToFile(PackageBytes, *LocalObject)))
	{
		return false;
	}

	const FString Pointer = FString::Printf(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:%s\nsize %d\n"), *Oid, PackageBytes.Num());
	FTCHARToUTF8 PointerUtf8(*Pointer);
	TArray<uint8> PointerBytes;
	PointerBytes.Append(reinterpret_cast<const uint8*>(PointerUtf8.Get()), PointerUtf8.Length());
	TArray<FAssetData> AssetData;
	FString FailureReason;
	const bool bLoaded = GitChangedAssetsMetadataTesting::LoadHeadMetadataBlob(
		GitBinary, RepositoryRoot, FixtureFilename, PointerBytes, AssetData, FailureReason);
	IFileManager::Get().DeleteDirectory(*RepositoryRoot, false, true);
	if (!TestTrue(TEXT("Local LFS object metadata loads through the logical .uasset filename"), bLoaded))
	{
		AddError(FailureReason);
		return false;
	}
	TestFalse(TEXT("Local LFS object exposes Asset Registry metadata"), AssetData.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsCatFileBatchAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.CatFileBatchProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsCatFileBatchAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestFalse(TEXT("Git executable is available"), GitBinary.IsEmpty()))
	{
		return false;
	}

	FGitCatFileBatchRequest InvalidRequest;
	const TCHAR EmbeddedNulSpec[] = TEXT("HEAD:invalid\0path");
	InvalidRequest.ObjectSpec = FString(UE_ARRAY_COUNT(EmbeddedNulSpec) - 1, EmbeddedNulSpec);
	FGitCatFileBatchRequest ValidRequest;
	ValidRequest.ObjectSpec = TEXT("HEAD:.editor-automation.env");
	TArray<FGitCatFileBatchResult> Results;
	FString Error;
	if (!TestTrue(TEXT("A malformed request does not cancel a valid neighboring cat-file request"),
		FGitCatFileBatchReader::ReadBlobs(GitBinary, FPaths::ProjectDir(), { InvalidRequest, ValidRequest }, Results, Error,
			1024 * 1024, 2 * 1024 * 1024, 30.0)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Cat-file preserves one result per request"), Results.Num(), 2);
	if (Results.Num() == 2)
	{
		TestTrue(TEXT("Embedded NUL is isolated to the malformed request"), Results[0].Error.Contains(TEXT("embedded NUL")));
		TestTrue(TEXT("Valid -Z contents header and payload are parsed"), Results[1].bFound && !Results[1].Data.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsDeletedOfpaHeadMetadataAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.DeletedOfpaHeadMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsDeletedOfpaHeadMetadataAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString RelativePath = TEXT("Plugins/GameFeatures/ShooterCore/Content/__ExternalActors__/Map/TestMaps2_OnlyPlayer/0/DI/4VLCH19HGZSLOVS16GF6U4.uasset");
	const FString PointerFilename = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("GitChangedAssetsDeletedOfpa.pointer"));
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	FString Error;
	if (!TestTrue(TEXT("Deleted OFPA HEAD blob can be exported"), GitSourceControlUtils::DumpRevisionBlobToFile(
		GitBinary, FPaths::ProjectDir(), FString::Printf(TEXT("HEAD:%s"), *RelativePath), PointerFilename, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<uint8> PointerBytes;
	const bool bPointerRead = FFileHelper::LoadFileToArray(PointerBytes, *PointerFilename);
	IFileManager::Get().Delete(*PointerFilename, false, true);
	if (!TestTrue(TEXT("Deleted OFPA LFS pointer can be read"), bPointerRead))
	{
		return false;
	}

	TArray<FAssetData> AssetData;
	const FString LogicalFilename = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RelativePath);
	if (!TestTrue(TEXT("Deleted OFPA local LFS package metadata can be loaded"),
		GitChangedAssetsMetadataTesting::LoadHeadMetadataBlob(GitBinary, FPaths::ProjectDir(), LogicalFilename, PointerBytes, AssetData, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestFalse(TEXT("Deleted OFPA header exposes Asset Registry data"), AssetData.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("Deleted OFPA AssetData contains a World Partition actor descriptor"),
		FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(AssetData[0]));

	int32 HeadReturnCode = -1;
	FString PinnedHead;
	FString HeadError;
	FPlatformProcess::ExecProcess(*GitBinary,
		*FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())),
		&HeadReturnCode, &PinnedHead, &HeadError);
	PinnedHead.TrimStartAndEndInline();
	if (!TestTrue(TEXT("Deleted OFPA production metadata fixture resolves HEAD"), HeadReturnCode == 0 && !PinnedHead.IsEmpty()))
	{
		AddError(HeadError);
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	Snapshot.GitBinary = GitBinary;
	Snapshot.RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Snapshot.PinnedHead = PinnedHead;
	FGitChangedAssetEntry& DeletedEntry = Snapshot.Entries.AddDefaulted_GetRef();
	DeletedEntry.RepositoryRelativePath = RelativePath;
	DeletedEntry.AbsoluteFilename = LogicalFilename;
	DeletedEntry.State = EGitChangedAssetState::Deleted;
	DeletedEntry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
	TArray<FGitChangedAssetHeadMetadataResult> HeadResults;
	TFuture<bool> HeadMetadataWorker = Async(EAsyncExecution::ThreadPool, [&Snapshot, &HeadResults, &Error]()
	{
		return FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(Snapshot, HeadResults, Error);
	});
	if (!TestTrue(TEXT("Deleted OFPA worker reads fixed-HEAD payload without Engine metadata APIs"), HeadMetadataWorker.Get()))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Deleted OFPA worker emits one fixed-HEAD payload"), HeadResults.Num(), 1);
	if (HeadResults.Num() == 1)
	{
		TestFalse(TEXT("Deleted OFPA worker payload retains the Git blob"), HeadResults[0].BlobData.IsEmpty());
		TestEqual(TEXT("Deleted OFPA worker payload retains the logical filename"), HeadResults[0].LogicalFilename, LogicalFilename);
	}
	FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(Snapshot, HeadResults);
	TestTrue(TEXT("Deleted OFPA production path resolves metadata"), Snapshot.Entries[0].bMetadataResolved);
	TestFalse(TEXT("Deleted OFPA production path resolves a friendly name"), Snapshot.Entries[0].DisplayName.IsEmpty());
	TestFalse(TEXT("Deleted OFPA production path resolves a concrete type"),
		Snapshot.Entries[0].AssetType.IsEmpty() || Snapshot.Entries[0].AssetType == TEXT("Unknown"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsDeletedRegularHeadMetadataAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.DeletedRegularHeadMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsDeletedRegularHeadMetadataAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString RelativePath = TEXT("Content/Blueprint/PCG_SO_Graph.uasset");
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	int32 HeadReturnCode = -1;
	FString PinnedHead;
	FString HeadError;
	FPlatformProcess::ExecProcess(*GitBinary,
		*FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())),
		&HeadReturnCode, &PinnedHead, &HeadError);
	PinnedHead.TrimStartAndEndInline();
	if (!TestTrue(TEXT("Deleted regular production metadata fixture resolves HEAD"), HeadReturnCode == 0 && !PinnedHead.IsEmpty()))
	{
		AddError(HeadError);
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	Snapshot.GitBinary = GitBinary;
	Snapshot.RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Snapshot.PinnedHead = PinnedHead;
	const FString LogicalFilename = FPaths::ConvertRelativePathToFull(Snapshot.RepositoryRoot, RelativePath);
	FGitChangedAssetEntry& DeletedEntry = Snapshot.Entries.AddDefaulted_GetRef();
	DeletedEntry.RepositoryRelativePath = RelativePath;
	DeletedEntry.AbsoluteFilename = LogicalFilename;
	DeletedEntry.State = EGitChangedAssetState::Deleted;
	DeletedEntry.PackageKind = EGitChangedAssetPackageKind::Regular;

	TArray<FGitChangedAssetHeadMetadataResult> HeadResults;
	FString Error;
	TFuture<bool> HeadMetadataWorker = Async(EAsyncExecution::ThreadPool, [&Snapshot, &HeadResults, &Error]()
	{
		return FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(Snapshot, HeadResults, Error);
	});
	if (!TestTrue(TEXT("Deleted regular worker reads fixed-HEAD payload"), HeadMetadataWorker.Get()))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Deleted regular worker emits one fixed-HEAD payload"), HeadResults.Num(), 1);
	if (HeadResults.Num() == 1)
	{
		TestFalse(TEXT("Deleted regular worker payload retains the Git blob"), HeadResults[0].BlobData.IsEmpty());
		TestEqual(TEXT("Deleted regular worker payload retains the logical filename"), HeadResults[0].LogicalFilename, LogicalFilename);
	}

	FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(Snapshot, HeadResults);
	TestTrue(TEXT("Deleted regular production path resolves metadata"), Snapshot.Entries[0].bMetadataResolved);
	TestFalse(TEXT("Deleted regular production path resolves a friendly name"), Snapshot.Entries[0].DisplayName.IsEmpty());
	TestFalse(TEXT("Deleted regular production path resolves a concrete type"),
		Snapshot.Entries[0].AssetType.IsEmpty() || Snapshot.Entries[0].AssetType == TEXT("Unknown"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsHeadMetadataChunkedApplyAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.ChunkedApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsHeadMetadataChunkedApplyAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FGitChangedAssetSnapshot Snapshot;
	TArray<FGitChangedAssetHeadMetadataResult> Results;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FGitChangedAssetEntry& Entry = Snapshot.Entries.AddDefaulted_GetRef();
		Entry.State = EGitChangedAssetState::Deleted;
		Entry.PackageKind = EGitChangedAssetPackageKind::Regular;

		FGitChangedAssetHeadMetadataResult& Result = Results.AddDefaulted_GetRef();
		Result.EntryIndex = Index;
		Result.FailureReason = FString::Printf(TEXT("Chunk fixture failure %d"), Index);
	}

	TestEqual(TEXT("First chunk applies exactly its requested result range"),
		FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadataRange(Snapshot, Results, 0, 2), 2);
	TestFalse(TEXT("First result was applied"), Snapshot.Entries[0].MetadataFailureReason.IsEmpty());
	TestFalse(TEXT("Second result was applied"), Snapshot.Entries[1].MetadataFailureReason.IsEmpty());
	TestTrue(TEXT("Later result is untouched until its own chunk"), Snapshot.Entries[2].MetadataFailureReason.IsEmpty());
	TestEqual(TEXT("Final chunk clamps to the remaining result count"),
		FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadataRange(Snapshot, Results, 2, 8), 1);
	TestFalse(TEXT("Third result was applied by its own chunk"), Snapshot.Entries[2].MetadataFailureReason.IsEmpty());
	TestEqual(TEXT("Exhausted range cannot reapply completed results"),
		FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadataRange(Snapshot, Results, 3, 1), 0);
	FGitChangedAssetsMetadataResolver::FinalizeHeadOnlyMetadata(Snapshot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsCurrentMetadataChunkedApplyAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.CurrentChunkedApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsCurrentMetadataChunkedApplyAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString Filename = GitChangedAssetsMetadataAutomationTestsPrivate::GetHeaderFallbackFixtureFilename();
	if (!TestTrue(TEXT("Current metadata chunk fixture exists"), FPaths::FileExists(Filename)))
	{
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FGitChangedAssetEntry& Entry = Snapshot.Entries.AddDefaulted_GetRef();
		Entry.AbsoluteFilename = Filename;
		Entry.RepositoryRelativePath = FString::Printf(TEXT("Content/CurrentChunk%d.uasset"), Index);
		Entry.State = EGitChangedAssetState::Modified;
	}
	FGitChangedAssetsMetadataResolver::BeginCurrentMetadata(Snapshot);
	TestEqual(TEXT("Current metadata range applies exactly one non-preemptible package header"),
		FGitChangedAssetsMetadataResolver::ApplyCurrentMetadataRange(Snapshot, 0, 1), 1);
	TestTrue(TEXT("First current metadata entry is resolved by its own chunk"), Snapshot.Entries[0].bMetadataResolved);
	TestFalse(TEXT("Second entry remains unresolved before its own chunk"), Snapshot.Entries[1].bMetadataResolved);
	TestEqual(TEXT("Second current metadata chunk completes remaining entry"),
		FGitChangedAssetsMetadataResolver::ApplyCurrentMetadataRange(Snapshot, 1, 8), 1);
	TestTrue(TEXT("Second current metadata entry resolves after its own chunk"), Snapshot.Entries[1].bMetadataResolved);
	TestEqual(TEXT("Current metadata range never reapplies exhausted entries"),
		FGitChangedAssetsMetadataResolver::ApplyCurrentMetadataRange(Snapshot, 2, 1), 0);
	FGitChangedAssetsMetadataResolver::FinalizeCurrentMetadata(Snapshot);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsExternalOwnerMetadataAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.ExternalOptionalOuter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsExternalOwnerMetadataAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FName PackageName(TEXT("/Game/__ExternalActors__/Maps/TestLevel/AB/CD/ActorPackage"));
	FAssetData ExternalActorData(PackageName, FName(TEXT("/Game/__ExternalActors__/Maps/TestLevel/AB/CD")), FName(TEXT("ActorPackage")),
		FTopLevelAssetPath(FName(TEXT("/Script/Engine")), FName(TEXT("StaticMeshActor"))));
#if WITH_EDITORONLY_DATA
	ExternalActorData.SetOptionalOuterPathName(FName(TEXT("/Game/Maps/TestLevel.TestLevel:PersistentLevel")));
#endif

	FGitChangedAssetEntry Entry;
	Entry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Maps/TestLevel/AB/CD/ActorPackage.uasset");
	GitChangedAssetsMetadataTesting::ApplyAssetData(ExternalActorData, Entry);

	TestEqual(TEXT("External AssetData corrects package name"), Entry.PackageName, FString(TEXT("/Game/__ExternalActors__/Maps/TestLevel/AB/CD/ActorPackage")));
	TestEqual(TEXT("OptionalOuterPath resolves owner level"), Entry.OwnerLevel, FString(TEXT("/Game/Maps/TestLevel")));
	TestTrue(TEXT("External owner is marked resolved"), Entry.bOwnerLevelResolved);
	TestEqual(TEXT("External package kind is preserved"), Entry.PackageKind, EGitChangedAssetPackageKind::ExternalActor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsHeadMetadataSourceAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.HeadSourceSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsHeadMetadataSourceAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FGitChangedAssetEntry DeletedReplacement;
	DeletedReplacement.State = EGitChangedAssetState::Deleted;
	DeletedReplacement.bHasUntrackedReplacement = true;
	DeletedReplacement.RepositoryRelativePath = TEXT("Content/DeletedActor.uasset");
	DeletedReplacement.AbsoluteFilename = TEXT("G:/Fixture/Content/DeletedActor.uasset");
	TestFalse(TEXT("A staged delete never reads an untracked replacement as current metadata"),
		GitChangedAssetsMetadataTesting::ShouldResolveCurrentFileMetadata(DeletedReplacement));
	FString RepositoryRelativePath;
	FString LogicalFilename;
	TestTrue(TEXT("Deleted metadata is always requested from fixed HEAD even when a replacement filename exists"),
		GitChangedAssetsMetadataTesting::GetHeadMetadataSourcePath(DeletedReplacement, true, RepositoryRelativePath, LogicalFilename));
	TestEqual(TEXT("Deleted metadata keeps the tracked repository path"), RepositoryRelativePath, DeletedReplacement.RepositoryRelativePath);
	TestEqual(TEXT("Deleted metadata keeps the tracked logical filename"), LogicalFilename, DeletedReplacement.AbsoluteFilename);

	FGitChangedAssetEntry RenameDeletedDestination;
	RenameDeletedDestination.State = EGitChangedAssetState::Renamed;
	RenameDeletedDestination.RepositoryRelativePath = TEXT("Content/NewActor.uasset");
	RenameDeletedDestination.AbsoluteFilename = TEXT("G:/Fixture/Content/NewActor.uasset");
	RenameDeletedDestination.RenameFromRepositoryRelativePath = TEXT("Content/OldActor.uasset");
	RenameDeletedDestination.RenameFromAbsoluteFilename = TEXT("G:/Fixture/Content/OldActor.uasset");
	RepositoryRelativePath.Reset();
	LogicalFilename.Reset();
	TestTrue(TEXT("Rename with a missing destination requests the old fixed-HEAD asset"),
		GitChangedAssetsMetadataTesting::GetHeadMetadataSourcePath(RenameDeletedDestination, false, RepositoryRelativePath, LogicalFilename));
	TestEqual(TEXT("Rename HEAD request uses old repository path"), RepositoryRelativePath, RenameDeletedDestination.RenameFromRepositoryRelativePath);
	TestEqual(TEXT("Rename HEAD archive uses old logical filename"), LogicalFilename, RenameDeletedDestination.RenameFromAbsoluteFilename);
	TestFalse(TEXT("Rename with an existing destination does not unnecessarily schedule HEAD metadata"),
		GitChangedAssetsMetadataTesting::GetHeadMetadataSourcePath(RenameDeletedDestination, true, RepositoryRelativePath, LogicalFilename));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsMissingNativeClassDescriptorAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.MissingNativeClassDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsMissingNativeClassDescriptorAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString RelativePath = TEXT("Plugins/GameFeatures/ShooterCore/Content/__ExternalActors__/Map/TestMaps2_OnlyPlayer/0/DI/4VLCH19HGZSLOVS16GF6U4.uasset");
	const FString PointerFilename = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("GitChangedAssetsMissingNativeClass.pointer"));
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	FString Error;
	if (!TestTrue(TEXT("Missing-native-class fixture can be exported"), GitSourceControlUtils::DumpRevisionBlobToFile(
		GitBinary, FPaths::ProjectDir(), FString::Printf(TEXT("HEAD:%s"), *RelativePath), PointerFilename, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<uint8> PointerBytes;
	const bool bPointerRead = FFileHelper::LoadFileToArray(PointerBytes, *PointerFilename);
	IFileManager::Get().Delete(*PointerFilename, false, true);
	if (!TestTrue(TEXT("Missing-native-class fixture pointer can be read"), bPointerRead))
	{
		return false;
	}

	TArray<FAssetData> OriginalAssetData;
	if (!TestTrue(TEXT("Missing-native-class fixture metadata can be loaded"),
		GitChangedAssetsMetadataTesting::LoadHeadMetadataBlob(GitSourceControlUtils::FindGitBinaryPath(), FPaths::ProjectDir(),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RelativePath), PointerBytes, OriginalAssetData, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestFalse(TEXT("Missing-native-class fixture contains AssetData"), OriginalAssetData.IsEmpty()))
	{
		return false;
	}

	FString ActorMetaData;
	if (!TestTrue(TEXT("Missing-native-class fixture contains raw ActorMetaData"),
		OriginalAssetData[0].GetTagValue(FWorldPartitionActorDescUtils::ActorMetaDataTagName(), ActorMetaData)))
	{
		return false;
	}
	FAssetDataTagMap Tags;
	Tags.Add(FWorldPartitionActorDescUtils::ActorMetaDataClassTagName(), TEXT("/Script/RemovedPlugin.RemovedActor"));
	Tags.Add(FWorldPartitionActorDescUtils::ActorMetaDataTagName(), MoveTemp(ActorMetaData));
	const FAssetData MissingNativeClassAssetData(
		OriginalAssetData[0].PackageName.ToString(), OriginalAssetData[0].GetObjectPathString(),
		OriginalAssetData[0].AssetClassPath, MoveTemp(Tags));

	FGitChangedAssetEntry Entry;
	Entry.RepositoryRelativePath = RelativePath;
	Entry.PackageKind = EGitChangedAssetPackageKind::ExternalActor;
	GitChangedAssetsMetadataTesting::ApplyAssetData(MissingNativeClassAssetData, Entry);
	TestTrue(TEXT("Missing native class still yields actor descriptor metadata"), Entry.bHasActorDescriptorMetadata);
	TestFalse(TEXT("Missing native class preserves actor display name"), Entry.DisplayName.IsEmpty());
	TestFalse(TEXT("Missing native class preserves actor object path"), Entry.ObjectPath.IsEmpty());
	TestFalse(TEXT("Missing native class preserves actor object name"), Entry.ActorObjectName.IsEmpty());
	TestTrue(TEXT("Missing native class does not report a descriptor construction failure"), Entry.MetadataFailureReason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsDataLayerDisplayAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.DataLayerDisplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsDataLayerDisplayAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	TestEqual(TEXT("Data Layer asset paths use the friendly asset name"),
		GitChangedAssetsMetadataTesting::FriendlyDataLayerName(TEXT("/Game/DataLayers/DL_Day.DL_Day")), TEXT("DL_Day"));
	TestEqual(TEXT("Legacy Data Layer instance names remain readable"),
		GitChangedAssetsMetadataTesting::FriendlyDataLayerName(TEXT("Gameplay")), TEXT("Gameplay"));
	TestEqual(TEXT("Unresolved private Data Layer metadata is explicit instead of an implementation name"),
		GitChangedAssetsMetadataTesting::FriendlyDataLayerName(
			TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.DataLayerAsset")), TEXT("Private Data Layer (unresolved)"));

	const TArray<FString> DataLayerNames = { TEXT("DL_Day"), TEXT("EDL_City"), TEXT("DL_Night"), TEXT("DL_Day") };
	TestEqual(TEXT("Data Layers prefix the actor in stable sorted order"),
		GitChangedAssetsMetadataTesting::BuildActorDisplayObjectPath(
			TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.Actor_UAID_123"), TEXT("Actor_UAID_123"), DataLayerNames),
		TEXT("DL_Day + DL_Night + EDL_City.Actor"));
	TestEqual(TEXT("Actors without Data Layers retain the PersistentLevel display"),
		GitChangedAssetsMetadataTesting::BuildActorDisplayObjectPath(
			TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.Actor_UAID_123"), TEXT("Actor_UAID_123"), {}),
		TEXT("PersistentLevel.Actor"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsPrivateDataLayerMappingAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.PrivateDataLayerMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsPrivateDataLayerMappingAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FName PrivateDataLayerPath(TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.DataLayerAsset"));
	const FName PublicDataLayerPath(TEXT("/Game/DataLayers/DL_Day.DL_Day"));
	const FName DeprecatedDataLayerName(TEXT("LegacyGameplay"));
	TestTrue(TEXT("Private Data Layer subobject paths require the WorldDataLayers descriptor"),
		GitChangedAssetsMetadataTesting::RequiresWorldDataLayersDescriptor(PrivateDataLayerPath));
	TestTrue(TEXT("Deprecated Data Layer instance names require the WorldDataLayers descriptor"),
		GitChangedAssetsMetadataTesting::RequiresWorldDataLayersDescriptor(DeprecatedDataLayerName));
	TestFalse(TEXT("Public Data Layer asset paths do not require descriptor lookup or LoadObject"),
		GitChangedAssetsMetadataTesting::RequiresWorldDataLayersDescriptor(PublicDataLayerPath));

	FGitChangedAssetEntry Entry;
	Entry.bHasActorDescriptorMetadata = true;
	Entry.ObjectPath = TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.Actor_UAID_123");
	Entry.ActorObjectName = TEXT("Actor");
	Entry.ActorDataLayerIdentifiers = { PrivateDataLayerPath, PublicDataLayerPath, DeprecatedDataLayerName };
	Entry.ExternalDataLayerAssetPath = FName(TEXT("/Game/External/EDL_City.EDL_City"));
	const TMap<FName, FString> ResolvedNames = {
		{ PrivateDataLayerPath, TEXT("PrivateGameplay") },
		{ DeprecatedDataLayerName, TEXT("LegacyGameplay") },
	};
	GitChangedAssetsMetadataTesting::ApplyResolvedDataLayerNames(Entry, ResolvedNames);
	TestEqual(TEXT("Private and deprecated mappings replace only exact identifiers and retain public/external names"),
		Entry.DisplayObjectPath, TEXT("DL_Day + EDL_City + LegacyGameplay + PrivateGameplay.Actor"));
	TestTrue(TEXT("Tooltip preserves exact private Data Layer path"), Entry.FullDataLayerNames.Contains(PrivateDataLayerPath.ToString(), ESearchCase::CaseSensitive));

	FWorldDataLayersActorDesc UninitializedDescriptor;
	TestFalse(TEXT("A constructible but uninitialized WorldDataLayers descriptor is never used for static-cast lookup"),
		GitChangedAssetsMetadataTesting::IsUsableWorldDataLayersDescriptor(UninitializedDescriptor));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsDataLayerSourceCacheAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.DataLayerSourceCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsDataLayerSourceCacheAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FName PrivateDataLayerPath(TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.DataLayerAsset"));
	FGitChangedAssetEntry Entry;
	Entry.bHasActorDescriptorMetadata = true;
	Entry.DataLayerMappingSource = EGitChangedAssetDataLayerMappingSource::Head;
	Entry.ObjectPath = TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.Actor_UAID_123");
	Entry.ActorObjectName = TEXT("Actor");
	Entry.ActorDataLayerIdentifiers = { PrivateDataLayerPath };
	const TMap<FName, FString> CurrentNames = { { PrivateDataLayerPath, TEXT("CurrentPrivateName") } };
	const TMap<FName, FString> EmptyHeadNames;
	GitChangedAssetsMetadataTesting::ApplySourceSpecificDataLayerNames(Entry, CurrentNames, EmptyHeadNames);
	TestEqual(TEXT("HEAD actor never leaks a changed current WorldDataLayers name when HEAD metadata is unavailable"),
		Entry.DisplayObjectPath, TEXT("Private Data Layer (unresolved).Actor"));

	const TMap<FName, FString> HeadNames = { { PrivateDataLayerPath, TEXT("HeadPrivateName") } };
	GitChangedAssetsMetadataTesting::ApplySourceSpecificDataLayerNames(Entry, CurrentNames, HeadNames);
	TestEqual(TEXT("HEAD actor uses only fixed-HEAD Data Layer names"), Entry.DisplayObjectPath, TEXT("HeadPrivateName.Actor"));

	FGitChangedAssetSnapshot Snapshot;
	FGitChangedAssetDataLayerOwnerCache& OwnerCache = Snapshot.DataLayerOwnerCaches.FindOrAdd(TEXT("/Game/Maps/TestMap"));
	OwnerCache.bWorldDataLayersIndexed = true;
	OwnerCache.Current.AttemptedIdentifiers.Add(PrivateDataLayerPath);
	Snapshot.DataLayerOwnerCaches.FindChecked(TEXT("/Game/Maps/TestMap")).Head.AttemptedIdentifiers.Add(FName(TEXT("LegacyLayer")));
	const FGitChangedAssetSnapshot CopiedSnapshot(Snapshot);
	const FGitChangedAssetDataLayerOwnerCache& CopiedOwnerCache = CopiedSnapshot.DataLayerOwnerCaches.FindChecked(TEXT("/Game/Maps/TestMap"));
	TestTrue(TEXT("Current attempted identifiers survive snapshot copy and prevent a repeated AR lookup"),
		CopiedOwnerCache.bWorldDataLayersIndexed && CopiedOwnerCache.Current.AttemptedIdentifiers.Contains(PrivateDataLayerPath));
	TestTrue(TEXT("HEAD attempted identifiers remain source-separated from current cache"),
		CopiedOwnerCache.Head.AttemptedIdentifiers.Contains(FName(TEXT("LegacyLayer"))) &&
		!CopiedOwnerCache.Current.AttemptedIdentifiers.Contains(FName(TEXT("LegacyLayer"))));

	FGitChangedAssetSnapshot HeadOnlyWorldDataLayersSnapshot;
	GitChangedAssetsMetadataTesting::SeedHeadOnlyWorldDataLayersIndex(HeadOnlyWorldDataLayersSnapshot, TEXT("/Game/Maps/DeletedMap"),
		TEXT("Content/__ExternalActors__/Maps/DeletedMap/AA/BB/WorldDataLayers.uasset"), EGitChangedAssetState::Deleted);
	GitChangedAssetsMetadataTesting::SeedHeadOnlyWorldDataLayersIndex(HeadOnlyWorldDataLayersSnapshot, TEXT("/Game/Maps/OldMap"),
		TEXT("Content/__ExternalActors__/Maps/OldMap/CC/DD/WorldDataLayers.uasset"), EGitChangedAssetState::Renamed);
	const FGitChangedAssetDataLayerOwnerCache& DeletedOwnerCache = HeadOnlyWorldDataLayersSnapshot.DataLayerOwnerCaches.FindChecked(TEXT("/Game/Maps/DeletedMap"));
	const FGitChangedAssetDataLayerOwnerCache& RenamedOwnerCache = HeadOnlyWorldDataLayersSnapshot.DataLayerOwnerCaches.FindChecked(TEXT("/Game/Maps/OldMap"));
	TestTrue(TEXT("Deleted WDL ordinary HEAD metadata seeds a head-only index without a live AR asset"),
		DeletedOwnerCache.WorldDataLayers.Num() == 1 && !DeletedOwnerCache.WorldDataLayers[0].bHasCurrentAssetData &&
		DeletedOwnerCache.Head.HeadWorldDataLayersAssetData.Contains(TEXT("Content/__ExternalActors__/Maps/DeletedMap/AA/BB/WorldDataLayers.uasset")));
	TestEqual(TEXT("Cross-owner rename seeds the old fixed-HEAD owner and preserves rename state"),
		RenamedOwnerCache.WorldDataLayers[0].State, EGitChangedAssetState::Renamed);

	FGitChangedAssetDataLayerOwnerCache AddedWorldDataLayersOwnerCache;
	FGitChangedAssetWorldDataLayersIndexEntry& AddedWorldDataLayers = AddedWorldDataLayersOwnerCache.WorldDataLayers.AddDefaulted_GetRef();
	AddedWorldDataLayers.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Maps/TestMap/EE/FF/AddedWorldDataLayers.uasset");
	AddedWorldDataLayers.bChangedRelativeToHead = true;
	AddedWorldDataLayers.State = EGitChangedAssetState::Added;
	AddedWorldDataLayersOwnerCache.Head.KnownAbsentWorldDataLayersRepositoryPaths.Add(AddedWorldDataLayers.RepositoryRelativePath);
	TestTrue(TEXT("Added WDL expected HEAD absence does not block other head descriptor mapping"),
		GitChangedAssetsMetadataTesting::HasCompleteHeadWorldDataLayersMetadata(AddedWorldDataLayersOwnerCache));

	FGitChangedAssetEntry LegacyOuterMissingEntry;
	LegacyOuterMissingEntry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Maps/LegacyMap/AA/BB/WorldDataLayers.uasset");
	LegacyOuterMissingEntry.OwnerLevel = TEXT("/Game/Maps/LegacyMap");
	LegacyOuterMissingEntry.bOwnerLevelResolved = true;
	FString LegacyOuterMissingOwner;
	TestTrue(TEXT("Legacy WDL metadata without OptionalOuterPath reuses the already resolved entry owner"),
		GitChangedAssetsMetadataTesting::ResolveHeadWorldDataLayersOwnerWithoutOuter(HeadOnlyWorldDataLayersSnapshot,
			LegacyOuterMissingEntry, LegacyOuterMissingOwner));
	TestEqual(TEXT("Legacy missing-outer owner remains source-consistent"), LegacyOuterMissingOwner, TEXT("/Game/Maps/LegacyMap"));

	FGitChangedAssetEntry RenamedCustomWorldDataLayersEntry;
	RenamedCustomWorldDataLayersEntry.State = EGitChangedAssetState::Renamed;
	RenamedCustomWorldDataLayersEntry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/ExternalDataLayer/NewOwner/AA/BB/WorldDataLayers.uasset");
	RenamedCustomWorldDataLayersEntry.RenameFromRepositoryRelativePath = TEXT("Content/__ExternalActors__/ExternalDataLayer/OldOwner/CC/DD/WorldDataLayers.uasset");
	RenamedCustomWorldDataLayersEntry.OwnerLevel = TEXT("/Game/Maps/CurrentOwner");
	RenamedCustomWorldDataLayersEntry.bOwnerLevelResolved = true;
	FString RenamedCustomWorldDataLayersOwner;
	TestFalse(TEXT("Rename with missing outer and custom/EDL topology never falls back to the current owner"),
		GitChangedAssetsMetadataTesting::ResolveHeadWorldDataLayersOwnerWithoutOuter(HeadOnlyWorldDataLayersSnapshot,
			RenamedCustomWorldDataLayersEntry, RenamedCustomWorldDataLayersOwner));

	FGitChangedAssetDataLayerOwnerCache CrossOwnerUnknownCache;
	FGitChangedAssetWorldDataLayersIndexEntry& CrossOwnerUnknownWdl = CrossOwnerUnknownCache.WorldDataLayers.AddDefaulted_GetRef();
	CrossOwnerUnknownWdl.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Maps/NewMap/GG/HH/WorldDataLayers.uasset");
	CrossOwnerUnknownWdl.bChangedRelativeToHead = true;
	CrossOwnerUnknownWdl.State = EGitChangedAssetState::Renamed;
	CrossOwnerUnknownCache.Head.UnavailableWorldDataLayersRepositoryPaths.Add(CrossOwnerUnknownWdl.RepositoryRelativePath);
	TestFalse(TEXT("Cross-owner WDL without a unique HEAD owner remains unavailable instead of defaulting to the current owner"),
		GitChangedAssetsMetadataTesting::HasCompleteHeadWorldDataLayersMetadata(CrossOwnerUnknownCache));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsDataLayerSourceStagesAutomationTest,
	"Cthulhu.GitSourceControl.ChangedAssets.Metadata.DataLayerSourceStages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsDataLayerSourceStagesAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	GitChangedAssetsMetadataAutomationTestsPrivate::FDataLayerMappingFixture Fixture;
	if (!TestTrue(TEXT("A private or deprecated WorldDataLayers fixture is available"),
		GitChangedAssetsMetadataAutomationTestsPrivate::FindPrivateOrDeprecatedDataLayerFixture(Fixture)))
	{
		return false;
	}

	FGitChangedAssetSnapshot Snapshot;
	Snapshot.RepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FGitChangedAssetDataLayerOwnerCache& OwnerCache = Snapshot.DataLayerOwnerCaches.FindOrAdd(Fixture.OwnerLevel);
	OwnerCache.bWorldDataLayersIndexed = true;
	FGitChangedAssetWorldDataLayersIndexEntry& WorldDataLayers = OwnerCache.WorldDataLayers.AddDefaulted_GetRef();
	WorldDataLayers.AssetData = Fixture.WorldDataLayersAssetData;
	WorldDataLayers.bHasCurrentAssetData = true;
	auto AddActorEntry = [&Snapshot, &Fixture](const EGitChangedAssetDataLayerMappingSource Source)
	{
		FGitChangedAssetEntry& Entry = Snapshot.Entries.AddDefaulted_GetRef();
		Entry.bHasActorDescriptorMetadata = true;
		Entry.bOwnerLevelResolved = true;
		Entry.OwnerLevel = Fixture.OwnerLevel;
		Entry.DataLayerMappingSource = Source;
		Entry.ActorDataLayerIdentifiers = { Fixture.Identifier };
		Entry.ObjectPath = TEXT("/Game/TestMaps/StageFixture.StageFixture:PersistentLevel.Actor_UAID_StageFixture");
		Entry.ActorObjectName = Source == EGitChangedAssetDataLayerMappingSource::Current ? TEXT("CurrentActor") : TEXT("HeadActor");
		return Snapshot.Entries.Num() - 1;
	};
	const int32 CurrentEntryIndex = AddActorEntry(EGitChangedAssetDataLayerMappingSource::Current);
	const int32 HeadEntryIndex = AddActorEntry(EGitChangedAssetDataLayerMappingSource::Head);

	FGitChangedAssetsMetadataResolver::FinalizeCurrentMetadata(Snapshot);
	TestEqual(TEXT("Current finalization resolves only the current actor's private or deprecated Data Layer name"),
		Snapshot.Entries[CurrentEntryIndex].DisplayObjectPath, Fixture.FriendlyName + TEXT(".CurrentActor"));
	TestTrue(TEXT("Current finalization leaves the HEAD actor display untouched until HEAD metadata finalization"),
		Snapshot.Entries[HeadEntryIndex].DisplayObjectPath.IsEmpty());

	FGitChangedAssetsMetadataResolver::FinalizeHeadOnlyMetadata(Snapshot);
	TestEqual(TEXT("HEAD finalization resolves the same snapshot-local name only for the HEAD actor"),
		Snapshot.Entries[HeadEntryIndex].DisplayObjectPath, Fixture.FriendlyName + TEXT(".HeadActor"));

	FGitChangedAssetSnapshot UnavailableHeadSnapshot;
	FGitChangedAssetDataLayerOwnerCache& UnavailableOwnerCache = UnavailableHeadSnapshot.DataLayerOwnerCaches.FindOrAdd(Fixture.OwnerLevel);
	UnavailableOwnerCache.bWorldDataLayersIndexed = true;
	UnavailableOwnerCache.Current.ResolvedNames.Add(Fixture.Identifier, Fixture.FriendlyName);
	FGitChangedAssetWorldDataLayersIndexEntry& UnavailableWorldDataLayers = UnavailableOwnerCache.WorldDataLayers.AddDefaulted_GetRef();
	UnavailableWorldDataLayers.RepositoryRelativePath = TEXT("Content/FixtureWorldDataLayers.uasset");
	UnavailableWorldDataLayers.AssetData = Fixture.WorldDataLayersAssetData;
	UnavailableWorldDataLayers.bHasCurrentAssetData = true;
	UnavailableWorldDataLayers.bChangedRelativeToHead = true;
	UnavailableOwnerCache.Head.UnavailableWorldDataLayersRepositoryPaths.Add(UnavailableWorldDataLayers.RepositoryRelativePath);
	FGitChangedAssetEntry& UnavailableHeadEntry = UnavailableHeadSnapshot.Entries.AddDefaulted_GetRef();
	UnavailableHeadEntry.bHasActorDescriptorMetadata = true;
	UnavailableHeadEntry.bOwnerLevelResolved = true;
	UnavailableHeadEntry.OwnerLevel = Fixture.OwnerLevel;
	UnavailableHeadEntry.DataLayerMappingSource = EGitChangedAssetDataLayerMappingSource::Head;
	UnavailableHeadEntry.ActorDataLayerIdentifiers = { Fixture.Identifier };
	UnavailableHeadEntry.ObjectPath = TEXT("/Game/TestMaps/StageFixture.StageFixture:PersistentLevel.Actor_UAID_UnavailableHead");
	UnavailableHeadEntry.ActorObjectName = TEXT("UnavailableHeadActor");
	FGitChangedAssetsMetadataResolver::FinalizeHeadOnlyMetadata(UnavailableHeadSnapshot);
	TestEqual(TEXT("Unavailable fixed-HEAD WDL metadata remains unresolved instead of leaking the current name"),
		UnavailableHeadEntry.DisplayObjectPath,
		GitChangedAssetsMetadataTesting::FriendlyDataLayerName(Fixture.Identifier.ToString()) + TEXT(".UnavailableHeadActor"));
	return true;
}

#endif
