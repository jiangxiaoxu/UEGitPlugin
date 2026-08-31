// Copyright (c) 2026

#include "GitSourceControlActorHistory.h"
#include "GitSourceControlUtils.h"

#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Components/ChildActorComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace GitSourceControlActorHistoryAutomationTestsPrivate
{
	FString NormalizeDirectory(const FString& InDirectory)
	{
		FString Result = FPaths::ConvertRelativePathToFull(InDirectory);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	FString QuoteGitArgument(const FString& InArgument)
	{
		FString Escaped = InArgument;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	bool IsSameOrUnderDirectory(const FString& InPath, const FString& InDirectory)
	{
		const FString Path = NormalizeDirectory(InPath);
		const FString Directory = NormalizeDirectory(InDirectory);
		return FPaths::IsSamePath(Path, Directory) || FPaths::IsUnderDirectory(Path, Directory);
	}

	class FGitFixture final
	{
	public:
		explicit FGitFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		~FGitFixture()
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
				Test.AddError(TEXT("Git executable is required for Actor History automation tests."));
				return false;
			}
			Root = NormalizeDirectory(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("GitSourceControlActorHistoryTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			return IsSafePath() && IFileManager::Get().MakeDirectory(*Root, true)
				&& RunGit(TEXT("init"))
				&& RunGit(TEXT("config user.name \"GitSourceControlActorHistoryTests\""))
				&& RunGit(TEXT("config user.email \"git-actor-history-tests@example.invalid\""));
		}

		bool RunGit(const FString& InArguments, FString& OutOutput) const
		{
			int32 ReturnCode = INDEX_NONE;
			FString Error;
			const FString CommandLine = FString::Printf(TEXT("-C %s --no-optional-locks %s"), *QuoteGitArgument(Root), *InArguments);
			OutOutput.Reset();
			FPlatformProcess::ExecProcess(*GitBinary, *CommandLine, &ReturnCode, &OutOutput, &Error);
			if (ReturnCode == 0)
			{
				return true;
			}
			Test.AddError(FString::Printf(TEXT("Fixture Git command failed (%d): %s\n%s"), ReturnCode, *CommandLine, *Error));
			return false;
		}

		bool RunGit(const FString& InArguments) const
		{
			FString Output;
			return RunGit(InArguments, Output);
		}

		bool CommitAll(const FString& InMessage) const
		{
			return RunGit(TEXT("add --all")) && RunGit(FString::Printf(TEXT("commit --no-gpg-sign -m %s"), *QuoteGitArgument(InMessage)));
		}

		const FString& GetRoot() const { return Root; }
		const FString& GetGitBinary() const { return GitBinary; }

	private:
		bool IsSafePath() const
		{
			const FString Parent = FPaths::Combine(NormalizeDirectory(FPlatformProcess::UserTempDir()), TEXT("GitSourceControlActorHistoryTests"));
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

	bool SavePackageForTest(FAutomationTestBase& Test, UPackage* Package, UObject* Asset, const FString& Filename)
	{
		if (Package == nullptr || Asset == nullptr)
		{
			return false;
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
		if (!bSaved)
		{
			Test.AddError(FString::Printf(TEXT("Could not save actor history fixture package: %s"), *Filename));
		}
		Package->SetDirtyFlag(false);
		return bSaved && FPaths::FileExists(Filename);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlActorHistoryTargetResolverAutomationTest,
	"UEGitPlugin.ActorHistory.TargetResolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlActorHistoryTargetResolverAutomationTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace GitSourceControlActorHistoryAutomationTestsPrivate;
	FGitFixture Fixture(*this);
	if (!Fixture.Initialize())
	{
		return false;
	}

	const FString ContentDirectory = FPaths::Combine(Fixture.GetRoot(), TEXT("Content"));
	if (!IFileManager::Get().MakeDirectory(*ContentDirectory, true))
	{
		AddError(TEXT("Could not create Actor History fixture content directory."));
		return false;
	}
	const FString MountRoot = FString::Printf(TEXT("/GitActorHistory_%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPackageName::RegisterMountPoint(MountRoot, ContentDirectory);
	ON_SCOPE_EXIT
	{
		FPackageName::UnRegisterMountPoint(MountRoot, ContentDirectory);
	};

	const FString OwnerPackageName = MountRoot + TEXT("Maps/HistoryMap");
	UPackage* const OwnerPackage = CreatePackage(*OwnerPackageName);
	UWorld* const OwnerWorld = OwnerPackage != nullptr
		? UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("HistoryMap")), OwnerPackage)
		: nullptr;
	if (!TestNotNull(TEXT("Creates an Editor owner world"), OwnerWorld))
	{
		return false;
	}
	const FString OwnerFilename = FPackageName::LongPackageNameToFilename(OwnerPackageName, FPackageName::GetMapPackageExtension());
	if (!SavePackageForTest(*this, OwnerPackage, OwnerWorld, OwnerFilename))
	{
		return false;
	}

	const TArray<FString> ExternalRoots = ULevel::GetExternalActorsPaths(OwnerPackageName);
	if (!TestFalse(TEXT("Owner world exposes no ambiguous external roots"), ExternalRoots.Num() != 1))
	{
		return false;
	}
	const FString ActorPackageName = ExternalRoots[0] + TEXT("/A/B/HistoryActor");
	UPackage* const ActorPackage = CreatePackage(*ActorPackageName);
	AActor* const MainActor = ActorPackage != nullptr ? OwnerWorld->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("Creates an external actor"), MainActor))
	{
		return false;
	}
	MainActor->SetPackageExternal(true, false, ActorPackage);
	const FString ActorFilename = FPackageName::LongPackageNameToFilename(ActorPackageName, FPackageName::GetAssetPackageExtension());
	if (!SavePackageForTest(*this, ActorPackage, MainActor, ActorFilename))
	{
		return false;
	}

	UTypedElementSelectionSet* const Selection = NewObject<UTypedElementSelectionSet>(GetTransientPackage());
	if (!TestNotNull(TEXT("Creates a typed actor selection set"), Selection))
	{
		return false;
	}
	const FTypedElementSelectionOptions SelectionOptions;
	auto SelectActor = [Selection, SelectionOptions](AActor* Actor, const bool bAppend)
	{
		if (!bAppend)
		{
			Selection->ClearSelection(SelectionOptions);
		}
		return Selection->SelectElement(UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor), SelectionOptions);
	};

	FGitActorHistoryTarget Target;
	FString FailureReason;
	SelectActor(MainActor, false);
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	if (!TestTrue(TEXT("A saved Editor-world OFPA main actor resolves for Git History"),
		GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason)))
	{
		AddError(FailureReason);
		return false;
	}
	FString ExpectedActorFilename = FPaths::ConvertRelativePathToFull(ActorFilename);
	FPaths::NormalizeFilename(ExpectedActorFilename);
	TestEqual(TEXT("Resolved actor target uses its external .uasset path"), Target.Filename, ExpectedActorFilename);
	TestTrue(TEXT("Resolved target retains the selected actor"), Target.Actor.Get() == MainActor);
	TestEqual(TEXT("Menu target resolution starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	FString CanonicalFilename;
	FailureReason.Reset();
	TestTrue(TEXT("External actor package resolves through its canonical package filename"),
		GitSourceControlActorHistory::ResolveExternalActorPackageFilename(*MainActor, CanonicalFilename, FailureReason));
	TestEqual(TEXT("Canonical package filename matches the saved actor target"), CanonicalFilename, ExpectedActorFilename);
	const FPackagePath SavedLoadedPath = ActorPackage->GetLoadedPath();
	const FString MismatchedFilename = FPaths::Combine(Fixture.GetRoot(), TEXT("Content/MismatchedActor.uasset"));
	ActorPackage->SetLoadedPath(FPackagePath::FromLocalPath(MismatchedFilename));
	FailureReason.Reset();
	TestFalse(TEXT("An external actor with a mismatched LoadedPath is rejected"),
		GitSourceControlActorHistory::ResolveExternalActorPackageFilename(*MainActor, CanonicalFilename, FailureReason));
	ActorPackage->SetLoadedPath(SavedLoadedPath);
	TestTrue(TEXT("Loaded external actor can be re-resolved by canonical filename"),
		GitSourceControlActorHistory::FindLoadedExternalActorForHistoryDiff(ExpectedActorFilename) == MainActor);
	TestNull(TEXT("An unloaded or unknown external actor filename does not load a package implicitly"),
		GitSourceControlActorHistory::FindLoadedExternalActorForHistoryDiff(MismatchedFilename));

	AActor* const RegularActor = NewObject<AActor>(OwnerWorld->PersistentLevel, TEXT("RegularActor"), RF_Transient);
	SelectActor(RegularActor, false);
	FailureReason.Reset();
	TestFalse(TEXT("A regular non-external actor is rejected"), GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason));

	UPackage* const UnsavedPackage = CreatePackage(*(ExternalRoots[0] + TEXT("/A/B/UnsavedActor")));
	AActor* const UnsavedActor = NewObject<AActor>(OwnerWorld->PersistentLevel, TEXT("UnsavedActor"), RF_Public | RF_Standalone);
	OwnerWorld->PersistentLevel->Actors.Add(UnsavedActor);
	UnsavedActor->SetPackageExternal(true, false, UnsavedPackage);
	SelectActor(UnsavedActor, false);
	FailureReason.Reset();
	TestFalse(TEXT("An external actor without a saved local package is rejected"), GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason));

	UPackage* const PieActorPackage = CreatePackage(*(ExternalRoots[0] + TEXT("/A/B/PieActor")));
	PieActorPackage->SetPackageFlags(PKG_PlayInEditor);
	AActor* const PieActor = NewObject<AActor>(OwnerWorld->PersistentLevel, TEXT("PieActor"), RF_Public | RF_Standalone);
	OwnerWorld->PersistentLevel->Actors.Add(PieActor);
	PieActor->SetPackageExternal(true, false, PieActorPackage);
	SelectActor(PieActor, false);
	FailureReason.Reset();
	TestFalse(TEXT("An external actor package marked for PIE is rejected"), GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason));

	AActor* const ChildHost = OwnerWorld->SpawnActor<AActor>();
	UChildActorComponent* const ChildComponent = ChildHost != nullptr
		? NewObject<UChildActorComponent>(ChildHost, TEXT("ChildComponent"))
		: nullptr;
	if (!TestNotNull(TEXT("Creates a ChildActorComponent for non-main actor coverage"), ChildComponent))
	{
		return false;
	}
	ChildHost->AddInstanceComponent(ChildComponent);
	ChildComponent->SetChildActorClass(APawn::StaticClass());
	ChildComponent->RegisterComponent();
	AActor* const ChildActor = ChildComponent->GetChildActor();
	if (!TestNotNull(TEXT("Spawns a ChildActorComponent child for non-main actor coverage"), ChildActor))
	{
		return false;
	}
	TestFalse(TEXT("ChildActorComponent child is not a main package actor"), ChildActor->IsMainPackageActor());
	SelectActor(ChildActor, false);
	FailureReason.Reset();
	TestFalse(TEXT("A ChildActorComponent child is rejected as a history target"), GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason));

	SelectActor(MainActor, false);
	SelectActor(RegularActor, true);
	FailureReason.Reset();
	return TestFalse(TEXT("A mixed or multi-actor selection is rejected"),
		GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(*Selection, Target, FailureReason));
}

#endif
