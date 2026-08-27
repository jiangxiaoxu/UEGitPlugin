// Copyright (c) 2026

#include "GitChangedAssetsMetadata.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/TopLevelAssetPath.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitChangedAssetsMetadataAutomationTestsPrivate
{
	FString GetHeaderFallbackFixtureFilename()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("B_LyraGameInstance.uasset"));
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

#endif
