// Copyright (c) 2026

#include "GitChangedAssetsMetadata.h"
#include "GitCatFileBatchReader.h"
#include "GitSourceControlUtils.h"

#include "AssetRegistry/AssetData.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/TopLevelAssetPath.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitChangedAssetsMetadataAutomationTestsPrivate
{
	FString GetHeaderFallbackFixtureFilename()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("B_LyraGameInstance.uasset"));
	}

	bool TestResults(FAutomationTestBase& Test, const TArray<FGitChangedAssetHeadMetadataResult>& Results)
	{
		if (Results.Num() != 1)
		{
			return false;
		}
		return Test.TestFalse(TEXT("Fixed-HEAD worker retains the Git blob"), Results[0].BlobData.IsEmpty());
	}

	bool ResolveFixedHeadMetadata(FAutomationTestBase& Test, const FString& RelativePath, const EGitChangedAssetPackageKind PackageKind, const bool bExpectActorDescriptor)
	{
		const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
		if (!Test.TestFalse(TEXT("Git executable is available for fixed-HEAD metadata"), GitBinary.IsEmpty()))
		{
			return false;
		}
		int32 HeadReturnCode = INDEX_NONE;
		FString PinnedHead;
		FString HeadError;
		FPlatformProcess::ExecProcess(*GitBinary,
			*FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())),
			&HeadReturnCode, &PinnedHead, &HeadError);
		PinnedHead.TrimStartAndEndInline();
		if (!Test.TestTrue(TEXT("Production fixture resolves a fixed HEAD"), HeadReturnCode == 0 && !PinnedHead.IsEmpty()))
		{
			Test.AddError(HeadError);
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
		DeletedEntry.PackageKind = PackageKind;

		TArray<FGitChangedAssetHeadMetadataResult> HeadResults;
		FString Error;
		TFuture<bool> HeadMetadataWorker = Async(EAsyncExecution::ThreadPool, [&Snapshot, &HeadResults, &Error]()
		{
			return FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(Snapshot, HeadResults, Error);
		});
		if (!Test.TestTrue(TEXT("Deleted metadata worker reads the pinned HEAD payload"), HeadMetadataWorker.Get()))
		{
			Test.AddError(Error);
			return false;
		}
		if (!Test.TestEqual(TEXT("Fixed-HEAD metadata emits one result"), HeadResults.Num(), 1)
			|| !TestResults(Test, HeadResults))
		{
			return false;
		}
		if (!Test.TestEqual(TEXT("Fixed-HEAD metadata keeps the logical filename"), HeadResults[0].LogicalFilename, LogicalFilename))
		{
			return false;
		}
		FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(Snapshot, HeadResults);
		if (!Test.TestTrue(TEXT("Deleted metadata resolves from fixed HEAD"), Snapshot.Entries[0].bMetadataResolved)
			|| !Test.TestFalse(TEXT("Fixed-HEAD metadata keeps a friendly display name"), Snapshot.Entries[0].DisplayName.IsEmpty())
			|| !Test.TestFalse(TEXT("Fixed-HEAD metadata resolves a concrete type"), Snapshot.Entries[0].AssetType.IsEmpty() || Snapshot.Entries[0].AssetType == TEXT("Unknown")))
		{
			return false;
		}
		return !bExpectActorDescriptor || Test.TestTrue(TEXT("Deleted OFPA metadata retains a World Partition actor descriptor"), Snapshot.Entries[0].bHasActorDescriptorMetadata);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitChangedAssetsFixedHeadSafetyAutomationTest,
	"UEGitPlugin.ChangedAssets.Metadata.FixedHeadSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitChangedAssetsFixedHeadSafetyAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitChangedAssetsMetadataAutomationTestsPrivate;

	// LFS package bytes 先通过逻辑文件名解析, 再进入 metadata 解析.
	const FString FixtureFilename = GetHeaderFallbackFixtureFilename();
	TArray<uint8> PackageBytes;
	if (!TestTrue(TEXT("LFS fixture package bytes can be read"), FFileHelper::LoadFileToArray(PackageBytes, *FixtureFilename)))
	{
		return false;
	}
	const FString Oid(TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	const FString RepositoryRoot = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("GitChangedAssetsFixedHeadSafety"));
	const FString LocalObject = FPaths::Combine(RepositoryRoot, TEXT(".git/lfs/objects/aa/aa"), Oid);
	const FString GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	if (!TestFalse(TEXT("Git executable is available for LFS metadata"), GitBinary.IsEmpty()))
	{
		return false;
	}
	IFileManager::Get().DeleteDirectory(*RepositoryRoot, false, true);
	IFileManager::Get().MakeDirectory(*RepositoryRoot, true);
	int32 GitReturnCode = INDEX_NONE;
	FString GitError;
	FPlatformProcess::ExecProcess(*GitBinary, *FString::Printf(TEXT("-C \"%s\" init --quiet"), *RepositoryRoot), &GitReturnCode, nullptr, &GitError);
	if (!TestTrue(TEXT("LFS fixture repository initializes"), GitReturnCode == 0))
	{
		AddError(GitError);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LocalObject), true);
	if (!TestTrue(TEXT("LFS object bytes can be written"), FFileHelper::SaveArrayToFile(PackageBytes, *LocalObject)))
	{
		return false;
	}
	const FString Pointer = FString::Printf(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:%s\nsize %d\n"), *Oid, PackageBytes.Num());
	FTCHARToUTF8 PointerUtf8(*Pointer);
	TArray<uint8> PointerBytes;
	PointerBytes.Append(reinterpret_cast<const uint8*>(PointerUtf8.Get()), PointerUtf8.Length());
	TArray<FAssetData> AssetData;
	FString FailureReason;
	const bool bLoaded = GitChangedAssetsMetadataTesting::LoadHeadMetadataBlob(GitBinary, RepositoryRoot, FixtureFilename, PointerBytes, AssetData, FailureReason);
	IFileManager::Get().DeleteDirectory(*RepositoryRoot, false, true);
	if (!TestTrue(TEXT("LFS package bytes load through the logical filename"), bLoaded))
	{
		AddError(FailureReason);
		return false;
	}
	if (!TestFalse(TEXT("LFS package bytes expose metadata"), AssetData.IsEmpty()))
	{
		return false;
	}

	// cat-file -Z 将 malformed request 与相邻有效 request 隔离.
	FGitCatFileBatchRequest InvalidRequest;
	const TCHAR EmbeddedNulSpec[] = TEXT("HEAD:invalid\0path");
	InvalidRequest.ObjectSpec = FString(UE_ARRAY_COUNT(EmbeddedNulSpec) - 1, EmbeddedNulSpec);
	FGitCatFileBatchRequest ValidRequest;
	ValidRequest.ObjectSpec = TEXT("HEAD:.editor-automation.env");
	TArray<FGitCatFileBatchResult> Results;
	FString Error;
	if (!TestTrue(TEXT("cat-file batch keeps a valid neighboring request"), FGitCatFileBatchReader::ReadBlobs(
		GitBinary, FPaths::ProjectDir(), { InvalidRequest, ValidRequest }, Results, Error, 1024 * 1024, 2 * 1024 * 1024, 30.0)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("cat-file batch returns one result per request"), Results.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("cat-file isolates the embedded NUL error"), Results[0].Error.Contains(TEXT("embedded NUL")));
	TestTrue(TEXT("cat-file parses the valid -Z payload"), Results[1].bFound && !Results[1].Data.IsEmpty());

	if (!ResolveFixedHeadMetadata(*this, TEXT("Content/Blueprint/PCG_SO_Graph.uasset"), EGitChangedAssetPackageKind::Regular, false)
		|| !ResolveFixedHeadMetadata(*this,
			TEXT("Plugins/GameFeatures/ShooterCore/Content/__ExternalActors__/Map/TestMaps2_OnlyPlayer/0/DI/4VLCH19HGZSLOVS16GF6U4.uasset"),
			EGitChangedAssetPackageKind::ExternalActor, true))
	{
		return false;
	}

	// A deleted entry and an untracked replacement must still source metadata from fixed HEAD.
	FGitChangedAssetEntry DeletedReplacement;
	DeletedReplacement.State = EGitChangedAssetState::Deleted;
	DeletedReplacement.bHasUntrackedReplacement = true;
	DeletedReplacement.RepositoryRelativePath = TEXT("Content/DeletedActor.uasset");
	DeletedReplacement.AbsoluteFilename = TEXT("G:/Fixture/Content/DeletedActor.uasset");
	TestFalse(TEXT("Deleted replacement never resolves current metadata"), GitChangedAssetsMetadataTesting::ShouldResolveCurrentFileMetadata(DeletedReplacement));
	FString RepositoryRelativePath;
	FString LogicalFilename;
	TestTrue(TEXT("Deleted replacement resolves the tracked HEAD path"), GitChangedAssetsMetadataTesting::GetHeadMetadataSourcePath(
		DeletedReplacement, true, RepositoryRelativePath, LogicalFilename));
	TestEqual(TEXT("Deleted replacement keeps the tracked repository path"), RepositoryRelativePath, DeletedReplacement.RepositoryRelativePath);
	TestEqual(TEXT("Deleted replacement keeps the tracked logical filename"), LogicalFilename, DeletedReplacement.AbsoluteFilename);

	FGitChangedAssetEntry ExternalEntry;
	ExternalEntry.RepositoryRelativePath = TEXT("Content/__ExternalActors__/Maps/TestLevel/AB/CD/ActorPackage.uasset");
	FAssetData ExternalActorData(FName(TEXT("/Game/__ExternalActors__/Maps/TestLevel/AB/CD/ActorPackage")),
		FName(TEXT("/Game/__ExternalActors__/Maps/TestLevel/AB/CD")), FName(TEXT("ActorPackage")),
		FTopLevelAssetPath(FName(TEXT("/Script/Engine")), FName(TEXT("StaticMeshActor"))));
#if WITH_EDITORONLY_DATA
	ExternalActorData.SetOptionalOuterPathName(FName(TEXT("/Game/Maps/TestLevel.TestLevel:PersistentLevel")));
#endif
	GitChangedAssetsMetadataTesting::ApplyAssetData(ExternalActorData, ExternalEntry);
	TestEqual(TEXT("OptionalOuter resolves the external owner level"), ExternalEntry.OwnerLevel, FString(TEXT("/Game/Maps/TestLevel")));
	TestTrue(TEXT("OptionalOuter marks the owner resolved"), ExternalEntry.bOwnerLevelResolved);
	TestEqual(TEXT("OptionalOuter preserves the external package kind"), ExternalEntry.PackageKind, EGitChangedAssetPackageKind::ExternalActor);
	return true;
}

#endif
