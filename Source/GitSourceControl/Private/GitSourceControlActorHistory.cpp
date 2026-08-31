// Copyright (c) 2026

#include "GitSourceControlActorHistory.h"

#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace GitSourceControlActorHistoryPrivate
{
	bool SetFailure(FString& OutFailureReason, const TCHAR* InReason)
	{
		OutFailureReason = InReason;
		return false;
	}
}

bool GitSourceControlActorHistory::ResolveExternalActorPackageFilename(const AActor& InActor, FString& OutFilename, FString& OutFailureReason)
{
	OutFilename.Reset();
	OutFailureReason.Reset();
	if (!IsValid(&InActor) || InActor.HasAnyFlags(RF_Transient) || !InActor.IsPackageExternal() || !InActor.IsMainPackageActor())
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("Git History is available only for external main actors."));
	}
	const UWorld* const World = InActor.GetWorld();
	if (World == nullptr || World->WorldType != EWorldType::Editor)
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("Git History is available only for actors in an Editor world."));
	}
	UPackage* const ActorPackage = InActor.GetSceneOutlinerItemPackage();
	if (ActorPackage == nullptr || ActorPackage->HasAnyFlags(RF_Transient) || ActorPackage->HasAnyPackageFlags(PKG_PlayInEditor | PKG_NewlyCreated))
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("The selected actor package is transient, newly created, or belongs to PIE."));
	}

	FString CanonicalFilename;
	if (!FPackageName::DoesPackageExist(ActorPackage->GetName(), &CanonicalFilename))
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("The selected actor package has not been saved to a local file."));
	}
	CanonicalFilename = FPaths::ConvertRelativePathToFull(CanonicalFilename);
	FPaths::NormalizeFilename(CanonicalFilename);
	if (!CanonicalFilename.EndsWith(FPackageName::GetAssetPackageExtension(), ESearchCase::IgnoreCase))
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("The selected actor package is not a saved .uasset file."));
	}

	FString LoadedFilename = ActorPackage->GetLoadedPath().GetLocalFullPath();
	if (!LoadedFilename.IsEmpty())
	{
		LoadedFilename = FPaths::ConvertRelativePathToFull(LoadedFilename);
		FPaths::NormalizeFilename(LoadedFilename);
		if (!FPaths::IsSamePath(LoadedFilename, CanonicalFilename))
		{
			return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("The selected actor package is being renamed or moved; save or complete the move before viewing Git History."));
		}
	}

	OutFilename = MoveTemp(CanonicalFilename);
	return true;
}

bool GitSourceControlActorHistory::ResolveExternalActorHistoryTarget(const UTypedElementSelectionSet& InSelection,
	FGitActorHistoryTarget& OutTarget, FString& OutFailureReason)
{
	OutTarget = {};
	OutFailureReason.Reset();
	if (InSelection.GetNumSelectedElements() != 1 || InSelection.CountSelectedObjects<AActor>() != 1)
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("Git History requires exactly one selected actor."));
	}

	AActor* const Actor = InSelection.GetTopSelectedObject<AActor>();
	if (!IsValid(Actor))
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("The selected actor is no longer loaded."));
	}
	const UWorld* const World = Actor->GetWorld();
	if (World == nullptr || World->WorldType != EWorldType::Editor)
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("Git History is available only for actors in an Editor world."));
	}
	if (Actor->HasAnyFlags(RF_Transient) || !Actor->IsPackageExternal() || !Actor->IsMainPackageActor())
	{
		return GitSourceControlActorHistoryPrivate::SetFailure(OutFailureReason, TEXT("Git History is available only for external main actors."));
	}

	FString Filename;
	if (!ResolveExternalActorPackageFilename(*Actor, Filename, OutFailureReason))
	{
		return false;
	}

	OutTarget.Filename = MoveTemp(Filename);
	OutTarget.Actor = Actor;
	return true;
}

AActor* GitSourceControlActorHistory::FindLoadedExternalActorForHistoryDiff(const FString& InCanonicalFilename)
{
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(InCanonicalFilename, PackageName))
	{
		return nullptr;
	}
	UPackage* const Package = FindPackage(nullptr, *PackageName);
	AActor* const Actor = Package != nullptr ? AActor::FindActorInPackage(Package) : nullptr;
	if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient) || !Actor->IsPackageExternal() || !Actor->IsMainPackageActor())
	{
		return nullptr;
	}
	const UWorld* const World = Actor->GetWorld();
	if (World == nullptr || World->WorldType != EWorldType::Editor || Actor->GetSceneOutlinerItemPackage() != Package)
	{
		return nullptr;
	}
	return Actor;
}

EGitHistoryDiffStrategy GitSourceControlActorHistory::GetDiffStrategy(const EGitHistoryTargetKind InTargetKind)
{
	return InTargetKind == EGitHistoryTargetKind::ExternalActor
		? EGitHistoryDiffStrategy::ActorDetails
		: EGitHistoryDiffStrategy::AssetTools;
}
