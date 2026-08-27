// Copyright (c) 2026
//
// Changed Assets 的 Asset Registry metadata 解析. 不加载 UObject, 不启动网络 Git/LFS 操作.

#include "GitChangedAssetsMetadata.h"

#include "GitCatFileBatchReader.h"
#include "GitLfsLocalObjectStore.h"
#include "GitSourceControlUtils.h"

#include "Algo/AllOf.h"
#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/ArchiveProxy.h"
#include "UObject/PrimaryAssetId.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/DataLayer/WorldDataLayersActorDesc.h"

namespace GitChangedAssetsMetadataPrivate
{
constexpr int64 HeadMetadataBlobLimitBytes = 1024 * 1024;
constexpr int64 HeadMetadataTotalBlobLimitBytes = 8 * 1024 * 1024;

class FLogicalFilenameArchiveProxy final : public FArchiveProxy
{
public:
	FLogicalFilenameArchiveProxy(FArchive& InInnerArchive, FString InLogicalFilename)
		: FArchiveProxy(InInnerArchive)
		, LogicalFilename(MoveTemp(InLogicalFilename))
	{
	}

	virtual FString GetArchiveName() const override
	{
		return LogicalFilename;
	}

private:
	FString LogicalFilename;
};

EGitChangedAssetPackageKind ClassifyPackageKind(const FString& InPackageName, const FString& InRepositoryRelativePath)
{
	const FString& Path = InPackageName.IsEmpty() ? InRepositoryRelativePath : InPackageName;
	if (Path.Contains(TEXT("/__ExternalActors__/"), ESearchCase::IgnoreCase))
	{
		return EGitChangedAssetPackageKind::ExternalActor;
	}
	if (Path.Contains(TEXT("/__ExternalObjects__/"), ESearchCase::IgnoreCase))
	{
		return EGitChangedAssetPackageKind::ExternalObject;
	}
	return InPackageName.IsEmpty() ? EGitChangedAssetPackageKind::Unknown : EGitChangedAssetPackageKind::Regular;
}

void SetMetadataFailure(FGitChangedAssetEntry& InOutEntry, const FString& InReason);

FString FriendlyDataLayerName(const FString& InDataLayerPath)
{
	FString Path = InDataLayerPath;
	Path.TrimStartAndEndInline();
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString ShortName = FPackageName::ObjectPathToObjectName(Path);
	if (ShortName.IsEmpty() || ShortName.Equals(Path, ESearchCase::CaseSensitive))
	{
		ShortName = FPackageName::GetShortName(Path);
	}
	// Private Data Layer instances serialize an asset with the fixed object name
	// DataLayerAsset. The owner WorldDataLayers descriptor resolves the actual
	// short name in a later snapshot-local pass. Before then, make the unresolved
	// state explicit instead of presenting the implementation name as metadata.
	if (ShortName.Equals(TEXT("DataLayerAsset"), ESearchCase::IgnoreCase)
		&& Path.Contains(TEXT(":PersistentLevel."), ESearchCase::IgnoreCase))
	{
		return TEXT("Private Data Layer (unresolved)");
	}
	return ShortName.IsEmpty() ? Path : ShortName;
}

FString BuildActorDisplayObjectPath(const FString& InObjectPath, const FString& InActorName,
	const TArray<FString>& InFriendlyDataLayerNames)
{
	FString ObjectPart = InObjectPath;
	int32 ObjectSeparator = INDEX_NONE;
	if (ObjectPart.FindLastChar(TEXT(':'), ObjectSeparator))
	{
		ObjectPart = ObjectPart.Mid(ObjectSeparator + 1);
	}

	const int32 UaidIndex = ObjectPart.Find(TEXT("_UAID_"), ESearchCase::IgnoreCase);
	if (UaidIndex != INDEX_NONE)
	{
		ObjectPart.LeftInline(UaidIndex, EAllowShrinking::No);
	}
	ObjectPart.TrimStartAndEndInline();

	FString ActorName = InActorName;
	const int32 ActorNameUaidIndex = ActorName.Find(TEXT("_UAID_"), ESearchCase::IgnoreCase);
	if (ActorNameUaidIndex != INDEX_NONE)
	{
		ActorName.LeftInline(ActorNameUaidIndex, EAllowShrinking::No);
	}
	int32 LevelSeparator = INDEX_NONE;
	if (ObjectPart.FindChar(TEXT('.'), LevelSeparator))
	{
		if (ActorName.IsEmpty())
		{
			ActorName = ObjectPart.Mid(LevelSeparator + 1);
		}
		ObjectPart = ObjectPart.Mid(LevelSeparator + 1);
	}
	if (ActorName.IsEmpty())
	{
		ActorName = ObjectPart;
	}
	if (ActorName.IsEmpty())
	{
		return ObjectPart;
	}

	TArray<FString> SortedDataLayerNames;
	for (const FString& DataLayerName : InFriendlyDataLayerNames)
	{
		if (!DataLayerName.IsEmpty())
		{
			SortedDataLayerNames.AddUnique(DataLayerName);
		}
	}
	SortedDataLayerNames.Sort();

	if (SortedDataLayerNames.IsEmpty())
	{
		// Preserve the standard PersistentLevel.Actor form when no Data Layer is assigned.
		if (ObjectPart.IsEmpty())
		{
			return FString::Printf(TEXT("PersistentLevel.%s"), *ActorName);
		}
		return ObjectPart.Contains(TEXT(".")) ? ObjectPart : FString::Printf(TEXT("PersistentLevel.%s"), *ActorName);
	}
	return FString::Printf(TEXT("%s.%s"), *FString::Join(SortedDataLayerNames, TEXT(" + ")), *ActorName);
}

bool IsPrivateDataLayerAssetTopology(const FName InDataLayerIdentifier)
{
	if (InDataLayerIdentifier.IsNone())
	{
		return false;
	}

	const FString Identifier = InDataLayerIdentifier.ToString();
	return Identifier.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
		&& Identifier.Contains(TEXT(":PersistentLevel."), ESearchCase::IgnoreCase);
}

bool IsDeprecatedDataLayerInstanceName(const FName InDataLayerIdentifier)
{
	if (InDataLayerIdentifier.IsNone())
	{
		return false;
	}

	// Actor descriptors only store a non-package FName when they use the deprecated
	// instance-name representation. Public DataLayer assets are long object paths.
	return !InDataLayerIdentifier.ToString().StartsWith(TEXT("/"), ESearchCase::CaseSensitive);
}

bool RequiresWorldDataLayersDescriptor(const FName InDataLayerIdentifier)
{
	return IsPrivateDataLayerAssetTopology(InDataLayerIdentifier)
		|| IsDeprecatedDataLayerInstanceName(InDataLayerIdentifier);
}

FString BuildFullDataLayerNames(const FGitChangedAssetEntry& InEntry)
{
	TArray<FString> FullNames;
	for (const FName DataLayerIdentifier : InEntry.ActorDataLayerIdentifiers)
	{
		if (!DataLayerIdentifier.IsNone())
		{
			FullNames.AddUnique(DataLayerIdentifier.ToString());
		}
	}
	if (!InEntry.ExternalDataLayerAssetPath.IsNone())
	{
		FullNames.AddUnique(InEntry.ExternalDataLayerAssetPath.ToString());
	}
	FullNames.Sort();
	return FString::Join(FullNames, TEXT(" + "));
}

FString ResolveFriendlyDataLayerName(const FName InDataLayerIdentifier, const TMap<FName, FString>* InResolvedNames)
{
	if (const FString* ResolvedName = InResolvedNames ? InResolvedNames->Find(InDataLayerIdentifier) : nullptr)
	{
		return *ResolvedName;
	}
	return FriendlyDataLayerName(InDataLayerIdentifier.ToString());
}

void RebuildActorDataLayerDisplay(FGitChangedAssetEntry& InOutEntry, const TMap<FName, FString>* InResolvedNames = nullptr)
{
	if (!InOutEntry.bHasActorDescriptorMetadata)
	{
		return;
	}

	TArray<FString> FriendlyDataLayerNames;
	FriendlyDataLayerNames.Reserve(InOutEntry.ActorDataLayerIdentifiers.Num() + 1);
	for (const FName DataLayerIdentifier : InOutEntry.ActorDataLayerIdentifiers)
	{
		const FString FriendlyName = ResolveFriendlyDataLayerName(DataLayerIdentifier, InResolvedNames);
		if (!FriendlyName.IsEmpty())
		{
			FriendlyDataLayerNames.AddUnique(FriendlyName);
		}
	}
	if (!InOutEntry.ExternalDataLayerAssetPath.IsNone())
	{
		const FString FriendlyName = FriendlyDataLayerName(InOutEntry.ExternalDataLayerAssetPath.ToString());
		if (!FriendlyName.IsEmpty())
		{
			FriendlyDataLayerNames.AddUnique(FriendlyName);
		}
	}
	FriendlyDataLayerNames.Sort();
	InOutEntry.FullDataLayerNames = BuildFullDataLayerNames(InOutEntry);
	InOutEntry.DisplayObjectPath = BuildActorDisplayObjectPath(InOutEntry.ObjectPath, InOutEntry.ActorObjectName, FriendlyDataLayerNames);
}

bool EnsurePackageName(FGitChangedAssetEntry& InOutEntry)
{
	if (!InOutEntry.AbsoluteFilename.IsEmpty())
	{
		FString MountedPackageName;
		FPackageName::EErrorCode MountedPathFailureReason = FPackageName::EErrorCode::PackageNameUnknown;
		if (FPackageName::TryConvertToMountedPath(InOutEntry.AbsoluteFilename, nullptr, &MountedPackageName,
			nullptr, nullptr, nullptr, nullptr, &MountedPathFailureReason))
		{
			InOutEntry.PackageName = MoveTemp(MountedPackageName);
		}
		else
		{
			FString FilenameConversionFailureReason;
			FString FallbackPackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(InOutEntry.AbsoluteFilename, FallbackPackageName, &FilenameConversionFailureReason))
			{
				InOutEntry.PackageName = MoveTemp(FallbackPackageName);
			}
			else
			{
				const FString MountedPathFailure = FPackageName::FormatErrorAsString(InOutEntry.AbsoluteFilename, MountedPathFailureReason);
				SetMetadataFailure(InOutEntry, FString::Printf(TEXT("无法将 .uasset 映射到已挂载 package path: %s; filename fallback: %s"),
					*MountedPathFailure, *FilenameConversionFailureReason));
			}
		}
	}
	InOutEntry.PackageKind = ClassifyPackageKind(InOutEntry.PackageName, InOutEntry.RepositoryRelativePath);
	return !InOutEntry.PackageName.IsEmpty();
}

void EnsureFallbackDisplay(FGitChangedAssetEntry& InOutEntry)
{
	EnsurePackageName(InOutEntry);
	if (InOutEntry.DisplayName.IsEmpty())
	{
		InOutEntry.DisplayName = FPaths::GetBaseFilename(InOutEntry.RepositoryRelativePath, true);
	}
	if (InOutEntry.ObjectPath.IsEmpty())
	{
		InOutEntry.ObjectPath = InOutEntry.PackageName.IsEmpty() ? InOutEntry.RepositoryRelativePath : InOutEntry.PackageName;
	}
	if (InOutEntry.AssetType.IsEmpty())
	{
		InOutEntry.AssetType = TEXT("Unknown");
	}
	if (InOutEntry.DisplayOwnerLevel.IsEmpty() && !InOutEntry.OwnerLevel.IsEmpty())
	{
		InOutEntry.DisplayOwnerLevel = FPackageName::GetShortName(InOutEntry.OwnerLevel);
	}
	if (InOutEntry.DisplayObjectPath.IsEmpty())
	{
		InOutEntry.DisplayObjectPath = InOutEntry.ObjectPath;
	}
}

void SetMetadataFailure(FGitChangedAssetEntry& InOutEntry, const FString& InReason)
{
	if (InOutEntry.MetadataFailureReason.IsEmpty())
	{
		InOutEntry.MetadataFailureReason = InReason;
	}
}

bool TryGetOwnerLevelFromOuterPath(const FAssetData& InAssetData, FString& OutOwnerLevel)
{
#if WITH_EDITORONLY_DATA
	FString OuterPath = InAssetData.GetOptionalOuterPathName().ToString();
	int32 ObjectSeparator = INDEX_NONE;
	if (!OuterPath.FindChar(TEXT('.'), ObjectSeparator))
	{
		return false;
	}
	OuterPath.LeftInline(ObjectSeparator, EAllowShrinking::No);
	if (!FPackageName::IsValidLongPackageName(OuterPath, true))
	{
		return false;
	}
	OutOwnerLevel = MoveTemp(OuterPath);
	return true;
#else
	return false;
#endif
}

bool TrySetOwnerLevelFromOuterPath(FGitChangedAssetEntry& InOutEntry, const FAssetData& InAssetData)
{
	FString OwnerLevel;
	if (!TryGetOwnerLevelFromOuterPath(InAssetData, OwnerLevel))
	{
		return false;
	}
	InOutEntry.OwnerLevel = MoveTemp(OwnerLevel);
	InOutEntry.bOwnerLevelResolved = true;
	return true;
}

TUniquePtr<FWorldPartitionActorDesc> TryCreateActorDescriptorFromAssetData(const FAssetData& InAssetData,
	FString& OutFailureReason)
{
	OutFailureReason.Reset();
	if (!FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(InAssetData))
	{
		OutFailureReason = TEXT("AssetData does not contain ActorMetaDataClass and ActorMetaData tags.");
		return nullptr;
	}

	FString ActorMetaDataClass;
	if (!InAssetData.GetTagValue(FWorldPartitionActorDescUtils::ActorMetaDataClassTagName(), ActorMetaDataClass)
		|| ActorMetaDataClass.TrimStartAndEnd().IsEmpty())
	{
		OutFailureReason = TEXT("Actor descriptor metadata is missing ActorMetaDataClass.");
		return nullptr;
	}

	FString ActorMetaData;
	if (!InAssetData.GetTagValue(FWorldPartitionActorDescUtils::ActorMetaDataTagName(), ActorMetaData)
		|| ActorMetaData.TrimStartAndEnd().IsEmpty())
	{
		OutFailureReason = TEXT("Actor descriptor metadata is missing ActorMetaData.");
		return nullptr;
	}

	FSoftObjectPath ActorPath = InAssetData.GetSoftObjectPath();
	ActorPath.FixupCoreRedirects();
	if (!ActorPath.IsValid())
	{
		OutFailureReason = FString::Printf(TEXT("Actor descriptor metadata has an invalid actor path for package '%s'."),
			*InAssetData.PackageName.ToString());
		return nullptr;
	}
	TArray<uint8> DecodedMetadata;
	if (!FBase64::Decode(ActorMetaData, DecodedMetadata))
	{
		OutFailureReason = FString::Printf(TEXT("Actor descriptor metadata has invalid base64 payload for '%s'."),
			*ActorPath.ToString());
		return nullptr;
	}

	// Engine implementation 在从 AssetData 构造 descriptor 时会解引用 native class;
	// 进入正常路径前必须先完成安全查找.
	if (FWorldPartitionActorDescUtils::GetActorNativeClassFromAssetData(InAssetData))
	{
		if (TUniquePtr<FWorldPartitionActorDesc> ActorDesc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(InAssetData))
		{
			return ActorDesc;
		}
	}

	// 即使 native class 已移除或其 module 未加载, 仍保留序列化 descriptor 数据和 actor path.
	// GetActorDescriptorFromInitParams() 无法解析 class 时会有意创建 AActor descriptor.
	FWorldPartitionActorDescUtils::FActorDescInitParams InitParams;
	InitParams.PathName = *ActorPath.ToString();
	InitParams.NativeClassName = *ActorMetaDataClass;
	InitParams.AssetData = MoveTemp(ActorMetaData);
	if (TUniquePtr<FWorldPartitionActorDesc> ActorDesc = FWorldPartitionActorDescUtils::GetActorDescriptorFromInitParams(
		InitParams, InAssetData.PackageName))
	{
		return ActorDesc;
	}

	OutFailureReason = FString::Printf(TEXT("Unable to construct actor descriptor for '%s' (native class '%s')."),
		*ActorPath.ToString(), *ActorMetaDataClass);
	return nullptr;
}

void ApplyAssetData(FGitChangedAssetEntry& InOutEntry, const FAssetData& InAssetData, const EGitChangedAssetMetadataSource InSource,
	const bool bAllowAssetDefinitionLookup)
{
	EnsureFallbackDisplay(InOutEntry);
	InOutEntry.DisplayOwnerLevel.Reset();
	InOutEntry.DisplayObjectPath.Reset();
	InOutEntry.FullDataLayerNames.Reset();
	InOutEntry.ActorDataLayerIdentifiers.Reset();
	InOutEntry.ExternalDataLayerAssetPath = NAME_None;
	InOutEntry.ActorObjectName.Reset();
	InOutEntry.bHasActorDescriptorMetadata = false;
	InOutEntry.DataLayerMappingSource = EGitChangedAssetDataLayerMappingSource::None;
	if (!InAssetData.PackageName.IsNone())
	{
		InOutEntry.PackageName = InAssetData.PackageName.ToString();
		InOutEntry.PackageKind = ClassifyPackageKind(InOutEntry.PackageName, InOutEntry.RepositoryRelativePath);
	}
	InOutEntry.MetadataSource = InSource;
	InOutEntry.bMetadataResolved = true;
	InOutEntry.MetadataFailureReason.Reset();

	const bool bIsActorDescriptor = FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(InAssetData);
	bool bApplyGenericAssetData = !bIsActorDescriptor;
	if (bIsActorDescriptor)
	{
		InOutEntry.PackageKind = InOutEntry.PackageKind == EGitChangedAssetPackageKind::Unknown
			? EGitChangedAssetPackageKind::ExternalActor
			: InOutEntry.PackageKind;
		FString ActorDescriptorFailureReason;
		if (TUniquePtr<FWorldPartitionActorDesc> ActorDesc = TryCreateActorDescriptorFromAssetData(InAssetData, ActorDescriptorFailureReason))
		{
			InOutEntry.bHasActorDescriptorMetadata = true;
			InOutEntry.DataLayerMappingSource = InSource == EGitChangedAssetMetadataSource::HeadPackageRegistry
				? EGitChangedAssetDataLayerMappingSource::Head
				: EGitChangedAssetDataLayerMappingSource::Current;
			FString ActorDisplayName;
			const FName ActorLabelOrName = ActorDesc->GetActorLabelOrName();
			if (!ActorLabelOrName.IsNone())
			{
				ActorDisplayName = ActorLabelOrName.ToString();
				InOutEntry.DisplayName = ActorDisplayName;
			}
			InOutEntry.ActorObjectName = ActorDesc->GetActorName().IsNone() ? ActorDisplayName : ActorDesc->GetActorName().ToString();
			const FSoftObjectPath ActorPath = ActorDesc->GetActorSoftPath();
			if (ActorPath.IsValid())
			{
				InOutEntry.ObjectPath = ActorPath.ToString();
			}
			const FName DisplayClassName = ActorDesc->GetDisplayClassName();
			if (!DisplayClassName.IsNone())
			{
				InOutEntry.AssetType = DisplayClassName.ToString();
			}

			InOutEntry.ActorDataLayerIdentifiers = ActorDesc->GetDataLayers(false);
			const FSoftObjectPath ExternalDataLayerAsset = ActorDesc->GetExternalDataLayerAsset();
			if (ExternalDataLayerAsset.IsValid())
			{
				InOutEntry.ExternalDataLayerAssetPath = FName(ExternalDataLayerAsset.GetAssetPath().ToString());
			}
			RebuildActorDataLayerDisplay(InOutEntry);
		}
		else
		{
			bApplyGenericAssetData = true;
			SetMetadataFailure(InOutEntry, ActorDescriptorFailureReason);
		}
	}
	if (bApplyGenericAssetData)
	{
		static const FName ActorLabelTag(TEXT("ActorLabel"));
		FString DisplayName;
		if (InAssetData.GetTagValue(ActorLabelTag, DisplayName) || InAssetData.GetTagValue(FPrimaryAssetId::PrimaryAssetDisplayNameTag, DisplayName))
		{
			InOutEntry.DisplayName = MoveTemp(DisplayName);
		}
		else if (!InAssetData.AssetName.IsNone())
		{
			InOutEntry.DisplayName = InAssetData.AssetName.ToString();
		}
		InOutEntry.ObjectPath = InAssetData.GetObjectPathString();
		if (bAllowAssetDefinitionLookup)
		{
			if (const UAssetDefinitionRegistry* DefinitionRegistry = UAssetDefinitionRegistry::Get())
			{
				if (const UAssetDefinition* Definition = DefinitionRegistry->GetAssetDefinitionForAsset(InAssetData))
				{
					const FText DisplayType = Definition->GetAssetDisplayName(InAssetData);
					if (!DisplayType.IsEmpty())
					{
						InOutEntry.AssetType = DisplayType.ToString();
					}
				}
			}
		}
		if (InOutEntry.AssetType.IsEmpty() || InOutEntry.AssetType == TEXT("Unknown"))
		{
			InOutEntry.AssetType = InAssetData.AssetClassPath.ToString();
		}
	}

	TrySetOwnerLevelFromOuterPath(InOutEntry, InAssetData);
	InOutEntry.DisplayOwnerLevel = InOutEntry.OwnerLevel.IsEmpty() ? FString() : FPackageName::GetShortName(InOutEntry.OwnerLevel);
	if (InOutEntry.DisplayObjectPath.IsEmpty())
	{
		InOutEntry.DisplayObjectPath = InOutEntry.ObjectPath;
	}
}

bool TryDeriveStandardExternalOwnerLevel(FGitChangedAssetEntry& InOutEntry)
{
	if (InOutEntry.PackageKind != EGitChangedAssetPackageKind::ExternalActor && InOutEntry.PackageKind != EGitChangedAssetPackageKind::ExternalObject)
	{
		return false;
	}
	const TCHAR* Marker = InOutEntry.PackageKind == EGitChangedAssetPackageKind::ExternalActor
		? TEXT("/__ExternalActors__/")
		: TEXT("/__ExternalObjects__/");
	const int32 MarkerIndex = InOutEntry.PackageName.Find(Marker, ESearchCase::IgnoreCase);
	if (MarkerIndex == INDEX_NONE)
	{
		return false;
	}

	const FString MountPoint = InOutEntry.PackageName.Left(MarkerIndex);
	const FString RelativeExternalPath = InOutEntry.PackageName.Mid(MarkerIndex + FCString::Strlen(Marker));
	TArray<FString> Parts;
	RelativeExternalPath.ParseIntoArray(Parts, TEXT("/"), true);
	// The standard Original/Reduced hash topology always terminates in two hash buckets and a leaf package.
	if (MountPoint.IsEmpty() || Parts.Num() < 4)
	{
		return false;
	}
	Parts.SetNum(Parts.Num() - 3, EAllowShrinking::No);
	const FString CandidateOwnerLevel = MountPoint + TEXT("/") + FString::Join(Parts, TEXT("/"));
	FString OwnerFilename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(CandidateOwnerLevel, OwnerFilename, FPackageName::GetMapPackageExtension()) ||
		!FPaths::FileExists(OwnerFilename))
	{
		return false;
	}
	InOutEntry.OwnerLevel = CandidateOwnerLevel;
	InOutEntry.bOwnerLevelResolved = true;
	InOutEntry.MetadataSource = EGitChangedAssetMetadataSource::DerivedExternalPath;
	return true;
}

void ResolveExternalOwnersByWorldRoots(FGitChangedAssetSnapshot& InOutSnapshot)
{
	bool bNeedsActorRoots = false;
	bool bNeedsObjectRoots = false;
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		if (!Entry.bOwnerLevelResolved)
		{
			bNeedsActorRoots |= Entry.PackageKind == EGitChangedAssetPackageKind::ExternalActor;
			bNeedsObjectRoots |= Entry.PackageKind == EGitChangedAssetPackageKind::ExternalObject;
		}
	}
	if (!bNeedsActorRoots && !bNeedsObjectRoots)
	{
		return;
	}

	FARFilter WorldFilter;
	WorldFilter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	WorldFilter.bRecursiveClasses = true;
	WorldFilter.bIncludeOnlyOnDiskAssets = true;
	TArray<FAssetData> WorldAssets;
	IAssetRegistry::GetChecked().GetAssets(WorldFilter, WorldAssets);

	struct FExternalRootOwner
	{
		FString RootPrefix;
		FString OwnerLevel;
		EGitChangedAssetPackageKind PackageKind = EGitChangedAssetPackageKind::Unknown;
	};
	TArray<FExternalRootOwner> RootOwners;
	for (const FAssetData& WorldAsset : WorldAssets)
	{
		const FString OwnerLevel = WorldAsset.PackageName.ToString();
		auto AddRoots = [&RootOwners, &OwnerLevel](const TArray<FString>& InRoots, const EGitChangedAssetPackageKind InPackageKind)
		{
			for (FString Root : InRoots)
			{
				Root.TrimStartAndEndInline();
				if (!Root.IsEmpty())
				{
					FExternalRootOwner& RootOwner = RootOwners.AddDefaulted_GetRef();
					RootOwner.RootPrefix = Root + TEXT("/");
					RootOwner.OwnerLevel = OwnerLevel;
					RootOwner.PackageKind = InPackageKind;
				}
			}
		};
		if (bNeedsActorRoots)
		{
			AddRoots(ULevel::GetExternalActorsPaths(OwnerLevel), EGitChangedAssetPackageKind::ExternalActor);
		}
		if (bNeedsObjectRoots)
		{
			AddRoots(ULevel::GetExternalObjectsPaths(OwnerLevel), EGitChangedAssetPackageKind::ExternalObject);
		}
	}
	RootOwners.Sort([](const FExternalRootOwner& Left, const FExternalRootOwner& Right)
	{
		return Left.RootPrefix.Len() > Right.RootPrefix.Len();
	});

	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		if (Entry.bOwnerLevelResolved || (Entry.PackageKind != EGitChangedAssetPackageKind::ExternalActor && Entry.PackageKind != EGitChangedAssetPackageKind::ExternalObject))
		{
			continue;
		}

		int32 BestPrefixLength = INDEX_NONE;
		TSet<FString> CandidateLevels;
		for (const FExternalRootOwner& RootOwner : RootOwners)
		{
			if (RootOwner.PackageKind != Entry.PackageKind)
			{
				continue;
			}
			if (BestPrefixLength != INDEX_NONE && RootOwner.RootPrefix.Len() < BestPrefixLength)
			{
				break;
			}
			if (Entry.PackageName.StartsWith(RootOwner.RootPrefix, ESearchCase::IgnoreCase))
			{
				if (RootOwner.RootPrefix.Len() > BestPrefixLength)
				{
					BestPrefixLength = RootOwner.RootPrefix.Len();
					CandidateLevels.Reset();
				}
				CandidateLevels.Add(RootOwner.OwnerLevel);
			}
		}
		if (CandidateLevels.Num() == 1)
		{
			Entry.OwnerLevel = CandidateLevels.Array()[0];
			Entry.bOwnerLevelResolved = true;
			Entry.MetadataSource = EGitChangedAssetMetadataSource::DerivedExternalPath;
		}
	}
}

const FWorldDataLayersActorDesc* TryGetWorldDataLayersActorDesc(const FWorldPartitionActorDesc& InActorDesc)
{
	// FWorldPartitionActorDescUtils builds the concrete descriptor from the native
	// class. Check that class before the static cast because descriptor metadata is
	// cache data and must not be trusted as a C++ type discriminator on its own.
	if (InActorDesc.GetNativeClass() != AWorldDataLayers::StaticClass()->GetClassPathName())
	{
		return nullptr;
	}

	const FWorldDataLayersActorDesc& WorldDataLayersDesc = static_cast<const FWorldDataLayersActorDesc&>(InActorDesc);
	return WorldDataLayersDesc.IsValid() ? &WorldDataLayersDesc : nullptr;
}

struct FOwnerDataLayerRequests
{
	TSet<FName> PrivateAssetPaths;
	TSet<FName> DeprecatedInstanceNames;

	bool IsEmpty() const
	{
		return PrivateAssetPaths.IsEmpty() && DeprecatedInstanceNames.IsEmpty();
	}
};

struct FDataLayerOwnerRoot
{
	FString Prefix;
	FString OwnerLevel;
};

void AddFriendlyDataLayerCandidate(FGitChangedAssetDataLayerMappingCache& InOutCache, const FName InIdentifier,
	const FDataLayerInstanceDesc* InDataLayerInstance)
{
	if (!InDataLayerInstance)
	{
		return;
	}

	// This call is intentionally limited to private asset topologies and deprecated
	// instance names. Calling GetShortName for a public UDataLayerAsset may load it.
	const FString ShortName = InDataLayerInstance->GetShortName();
	if (!ShortName.IsEmpty() && !ShortName.Equals(TEXT("Unknown"), ESearchCase::CaseSensitive))
	{
		InOutCache.FriendlyNameCandidates.FindOrAdd(InIdentifier).Add(ShortName);
	}
}

FString GetDataLayerOwnerFromAssetData(const FAssetData& InAssetData, const TArray<FDataLayerOwnerRoot>& InOwnerRoots,
	const TSet<FString>& InRequestedOwnerLevels)
{
	FString OwnerLevel;
	if (TryGetOwnerLevelFromOuterPath(InAssetData, OwnerLevel) && InRequestedOwnerLevels.Contains(OwnerLevel))
	{
		return OwnerLevel;
	}

	int32 BestPrefixLength = INDEX_NONE;
	TSet<FString> CandidateOwners;
	const FString PackageName = InAssetData.PackageName.ToString();
	for (const FDataLayerOwnerRoot& Root : InOwnerRoots)
	{
		if (BestPrefixLength != INDEX_NONE && Root.Prefix.Len() < BestPrefixLength)
		{
			break;
		}
		if (PackageName.StartsWith(Root.Prefix, ESearchCase::IgnoreCase))
		{
			if (Root.Prefix.Len() > BestPrefixLength)
			{
				BestPrefixLength = Root.Prefix.Len();
				CandidateOwners.Reset();
			}
			CandidateOwners.Add(Root.OwnerLevel);
		}
	}
	return CandidateOwners.Num() == 1 ? CandidateOwners.Array()[0] : FString();
}

const FGitChangedAssetEntry* FindChangedEntryForWorldDataLayersPackage(const FGitChangedAssetSnapshot& InSnapshot, const FName InPackageName)
{
	for (const FGitChangedAssetEntry& Entry : InSnapshot.Entries)
	{
		if (Entry.PackageName.Equals(InPackageName.ToString(), ESearchCase::CaseSensitive))
		{
			return &Entry;
		}
	}
	return nullptr;
}

bool TryGetRepositoryRelativeUassetPath(const FGitChangedAssetSnapshot& InSnapshot, const FName InPackageName, FString& OutRepositoryRelativePath)
{
	OutRepositoryRelativePath.Reset();
	if (InSnapshot.RepositoryRoot.IsEmpty())
	{
		return false;
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(InPackageName.ToString(), Filename, FPackageName::GetAssetPackageExtension()) ||
		!FPaths::MakePathRelativeTo(Filename, *InSnapshot.RepositoryRoot))
	{
		return false;
	}
	FPaths::MakeStandardFilename(Filename);
	if (!IsGitChangedAssetUassetPath(Filename) || Filename.StartsWith(TEXT("../"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	OutRepositoryRelativePath = MoveTemp(Filename);
	return true;
}

FString GetWorldDataLayersRepositoryKey(const FGitChangedAssetWorldDataLayersIndexEntry& InEntry)
{
	return InEntry.RepositoryRelativePath.IsEmpty()
		? FString::Printf(TEXT("<unmapped>:%s"), *InEntry.AssetData.PackageName.ToString())
		: InEntry.RepositoryRelativePath;
}

void IndexWorldDataLayersForOwners(FGitChangedAssetSnapshot& InOutSnapshot, const TSet<FString>& InOwnerLevels)
{
	TSet<FString> MissingOwnerLevels;
	for (const FString& OwnerLevel : InOwnerLevels)
	{
		if (!OwnerLevel.IsEmpty() && !InOutSnapshot.DataLayerOwnerCaches.FindOrAdd(OwnerLevel).bWorldDataLayersIndexed)
		{
			MissingOwnerLevels.Add(OwnerLevel);
		}
	}
	if (MissingOwnerLevels.IsEmpty())
	{
		return;
	}

	FARFilter WorldDataLayersFilter;
	WorldDataLayersFilter.ClassPaths.Add(AWorldDataLayers::StaticClass()->GetClassPathName());
	WorldDataLayersFilter.bRecursiveClasses = true;
	WorldDataLayersFilter.bRecursivePaths = true;
	WorldDataLayersFilter.bIncludeOnlyOnDiskAssets = true;

	TArray<FDataLayerOwnerRoot> OwnerRoots;
	for (const FString& OwnerLevel : MissingOwnerLevels)
	{
		FGitChangedAssetDataLayerOwnerCache& Cache = InOutSnapshot.DataLayerOwnerCaches.FindChecked(OwnerLevel);
		Cache.bWorldDataLayersIndexed = true;
		for (FString Root : ULevel::GetExternalActorsPaths(OwnerLevel))
		{
			Root.TrimStartAndEndInline();
			Root.RemoveFromEnd(TEXT("/"));
			if (Root.IsEmpty())
			{
				continue;
			}

			WorldDataLayersFilter.PackagePaths.AddUnique(FName(*Root));
			FDataLayerOwnerRoot& OwnerRoot = OwnerRoots.AddDefaulted_GetRef();
			OwnerRoot.Prefix = Root + TEXT("/");
			OwnerRoot.OwnerLevel = OwnerLevel;
		}
	}
	if (WorldDataLayersFilter.PackagePaths.IsEmpty())
	{
		return;
	}
	OwnerRoots.Sort([](const FDataLayerOwnerRoot& Left, const FDataLayerOwnerRoot& Right)
	{
		return Left.Prefix.Len() > Right.Prefix.Len();
	});

	TArray<FAssetData> WorldDataLayersAssets;
	// External actor packages are commonly AR-filtered. This is a single batched
	// query and deliberately opts in to those assets without scanning or loading a map.
	IAssetRegistry::GetChecked().GetAssets(WorldDataLayersFilter, WorldDataLayersAssets, false);
	TSet<FName> IndexedPackages;
	for (const FAssetData& WorldDataLayersAsset : WorldDataLayersAssets)
	{
		const FString OwnerLevel = GetDataLayerOwnerFromAssetData(WorldDataLayersAsset, OwnerRoots, MissingOwnerLevels);
		if (OwnerLevel.IsEmpty())
		{
			continue;
		}
		FGitChangedAssetDataLayerOwnerCache& Cache = InOutSnapshot.DataLayerOwnerCaches.FindChecked(OwnerLevel);
		if (IndexedPackages.Contains(WorldDataLayersAsset.PackageName))
		{
			continue;
		}
		IndexedPackages.Add(WorldDataLayersAsset.PackageName);
		FGitChangedAssetWorldDataLayersIndexEntry& IndexEntry = Cache.WorldDataLayers.AddDefaulted_GetRef();
		IndexEntry.AssetData = WorldDataLayersAsset;
		IndexEntry.bHasCurrentAssetData = true;
		if (const FGitChangedAssetEntry* ChangedEntry = FindChangedEntryForWorldDataLayersPackage(InOutSnapshot, WorldDataLayersAsset.PackageName))
		{
			IndexEntry.bChangedRelativeToHead = true;
			IndexEntry.State = ChangedEntry->State;
			if (ChangedEntry->IsRename() && IsGitChangedAssetUassetPath(ChangedEntry->RenameFromRepositoryRelativePath))
			{
				IndexEntry.RepositoryRelativePath = ChangedEntry->RenameFromRepositoryRelativePath;
			}
		}
		if (IndexEntry.RepositoryRelativePath.IsEmpty())
		{
			TryGetRepositoryRelativeUassetPath(InOutSnapshot, WorldDataLayersAsset.PackageName, IndexEntry.RepositoryRelativePath);
		}
		const FString RepositoryKey = GetWorldDataLayersRepositoryKey(IndexEntry);
		if (IndexEntry.bChangedRelativeToHead && (IndexEntry.State == EGitChangedAssetState::Added || IndexEntry.State == EGitChangedAssetState::Untracked))
		{
			Cache.Head.KnownAbsentWorldDataLayersRepositoryPaths.Add(RepositoryKey);
		}
		else if (IndexEntry.bChangedRelativeToHead && IndexEntry.RepositoryRelativePath.IsEmpty())
		{
			Cache.Head.UnavailableWorldDataLayersRepositoryPaths.Add(RepositoryKey);
		}
	}
}

void AddDataLayerCandidatesFromAssetData(const FAssetData& InAssetData, const FOwnerDataLayerRequests& InRequests,
	FGitChangedAssetDataLayerMappingCache& InOutCache)
{
	if (!FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(InAssetData))
	{
		return;
	}
	FString ActorDescriptorFailureReason;
	TUniquePtr<FWorldPartitionActorDesc> ActorDesc = TryCreateActorDescriptorFromAssetData(InAssetData, ActorDescriptorFailureReason);
	if (!ActorDesc)
	{
		return;
	}
	const FWorldDataLayersActorDesc* WorldDataLayersDesc = TryGetWorldDataLayersActorDesc(*ActorDesc);
	if (!WorldDataLayersDesc)
	{
		return;
	}
	for (const FName PrivateAssetPath : InRequests.PrivateAssetPaths)
	{
		AddFriendlyDataLayerCandidate(InOutCache, PrivateAssetPath,
			WorldDataLayersDesc->GetDataLayerInstanceFromAssetPath(PrivateAssetPath));
	}
	for (const FName DeprecatedInstanceName : InRequests.DeprecatedInstanceNames)
	{
		AddFriendlyDataLayerCandidate(InOutCache, DeprecatedInstanceName,
			WorldDataLayersDesc->GetDataLayerInstanceFromInstanceName(DeprecatedInstanceName));
	}
}


bool HasCompleteHeadWorldDataLayersMetadata(const FGitChangedAssetDataLayerOwnerCache& InCache)
{
	for (const FGitChangedAssetWorldDataLayersIndexEntry& IndexEntry : InCache.WorldDataLayers)
	{
		if (!IndexEntry.bChangedRelativeToHead)
		{
			continue;
		}
		const FString RepositoryKey = GetWorldDataLayersRepositoryKey(IndexEntry);
		if (IndexEntry.bKnownAbsentAtHead || IndexEntry.State == EGitChangedAssetState::Added || IndexEntry.State == EGitChangedAssetState::Untracked)
		{
			if (!InCache.Head.KnownAbsentWorldDataLayersRepositoryPaths.Contains(RepositoryKey))
			{
				return false;
			}
			continue;
		}
		if (InCache.Head.UnavailableWorldDataLayersRepositoryPaths.Contains(RepositoryKey) ||
			!InCache.Head.HeadWorldDataLayersAssetData.Contains(RepositoryKey))
		{
			return false;
		}
	}
	return true;
}

FGitChangedAssetDataLayerMappingCache& GetDataLayerMappingCache(FGitChangedAssetDataLayerOwnerCache& InOutOwnerCache,
	const EGitChangedAssetDataLayerMappingSource InSource)
{
	return InSource == EGitChangedAssetDataLayerMappingSource::Head ? InOutOwnerCache.Head : InOutOwnerCache.Current;
}

void ResolveDataLayerMappingForOwner(FGitChangedAssetDataLayerOwnerCache& InOutOwnerCache,
	const EGitChangedAssetDataLayerMappingSource InSource, const FOwnerDataLayerRequests& InRequests, const bool bForceRebuild)
{
	FGitChangedAssetDataLayerMappingCache& MappingCache = GetDataLayerMappingCache(InOutOwnerCache, InSource);
	TSet<FName> RequestedIdentifiers = InRequests.PrivateAssetPaths;
	RequestedIdentifiers.Append(InRequests.DeprecatedInstanceNames);
	if (RequestedIdentifiers.IsEmpty())
	{
		return;
	}

	TSet<FName> IdentifiersToResolve;
	if (bForceRebuild)
	{
		IdentifiersToResolve = RequestedIdentifiers;
	}
	else
	{
		for (const FName Identifier : RequestedIdentifiers)
		{
			if (!MappingCache.AttemptedIdentifiers.Contains(Identifier))
			{
				IdentifiersToResolve.Add(Identifier);
			}
		}
	}
	if (IdentifiersToResolve.IsEmpty())
	{
		return;
	}
	MappingCache.AttemptedIdentifiers.Append(IdentifiersToResolve);

	FOwnerDataLayerRequests RequestsToResolve;
	for (const FName Identifier : IdentifiersToResolve)
	{
		if (InRequests.PrivateAssetPaths.Contains(Identifier))
		{
			RequestsToResolve.PrivateAssetPaths.Add(Identifier);
		}
		else if (InRequests.DeprecatedInstanceNames.Contains(Identifier))
		{
			RequestsToResolve.DeprecatedInstanceNames.Add(Identifier);
		}
	}

	if (InSource == EGitChangedAssetDataLayerMappingSource::Head && !HasCompleteHeadWorldDataLayersMetadata(InOutOwnerCache))
	{
		return;
	}

	for (const FGitChangedAssetWorldDataLayersIndexEntry& IndexEntry : InOutOwnerCache.WorldDataLayers)
	{
		if (InSource == EGitChangedAssetDataLayerMappingSource::Current && !IndexEntry.bHasCurrentAssetData)
		{
			continue;
		}
		if (InSource == EGitChangedAssetDataLayerMappingSource::Head && IndexEntry.bChangedRelativeToHead)
		{
			if (IndexEntry.bKnownAbsentAtHead || IndexEntry.State == EGitChangedAssetState::Added || IndexEntry.State == EGitChangedAssetState::Untracked)
			{
				continue;
			}
			const FAssetData* HeadAssetData = MappingCache.HeadWorldDataLayersAssetData.Find(GetWorldDataLayersRepositoryKey(IndexEntry));
			if (HeadAssetData)
			{
				AddDataLayerCandidatesFromAssetData(*HeadAssetData, RequestsToResolve, MappingCache);
			}
		}
		else
		{
			AddDataLayerCandidatesFromAssetData(IndexEntry.AssetData, RequestsToResolve, MappingCache);
		}
	}

	for (const FName Identifier : IdentifiersToResolve)
	{
		if (const TSet<FString>* CandidateNames = MappingCache.FriendlyNameCandidates.Find(Identifier); CandidateNames && CandidateNames->Num() == 1)
		{
			MappingCache.ResolvedNames.Add(Identifier, CandidateNames->Array()[0]);
		}
		else
		{
			MappingCache.ResolvedNames.Remove(Identifier);
		}
	}
}

void RebuildCachedDataLayerDisplays(FGitChangedAssetSnapshot& InOutSnapshot)
{
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		if (!Entry.bHasActorDescriptorMetadata || !Entry.bOwnerLevelResolved || Entry.OwnerLevel.IsEmpty())
		{
			continue;
		}
		if (const FGitChangedAssetDataLayerOwnerCache* OwnerCache = InOutSnapshot.DataLayerOwnerCaches.Find(Entry.OwnerLevel))
		{
			const FGitChangedAssetDataLayerMappingCache& MappingCache = Entry.DataLayerMappingSource == EGitChangedAssetDataLayerMappingSource::Head
				? OwnerCache->Head
				: OwnerCache->Current;
			RebuildActorDataLayerDisplay(Entry, &MappingCache.ResolvedNames);
		}
	}
}

void ResolvePrivateAndDeprecatedDataLayerNames(FGitChangedAssetSnapshot& InOutSnapshot, const bool bForceRebuild = false)
{
	TSet<FString> OwnerLevelsToIndex;
	TMap<FString, FOwnerDataLayerRequests> RequestsByOwner;
	for (const FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		if (Entry.bOwnerLevelResolved && !Entry.OwnerLevel.IsEmpty())
		{
			OwnerLevelsToIndex.Add(Entry.OwnerLevel);
		}
		if (!Entry.bHasActorDescriptorMetadata || !Entry.bOwnerLevelResolved || Entry.OwnerLevel.IsEmpty())
		{
			continue;
		}
		FOwnerDataLayerRequests& Requests = RequestsByOwner.FindOrAdd(Entry.OwnerLevel);
		for (const FName DataLayerIdentifier : Entry.ActorDataLayerIdentifiers)
		{
			if (IsPrivateDataLayerAssetTopology(DataLayerIdentifier))
			{
				Requests.PrivateAssetPaths.Add(DataLayerIdentifier);
			}
			else if (IsDeprecatedDataLayerInstanceName(DataLayerIdentifier))
			{
				Requests.DeprecatedInstanceNames.Add(DataLayerIdentifier);
			}
		}
	}
	IndexWorldDataLayersForOwners(InOutSnapshot, OwnerLevelsToIndex);
	for (const TPair<FString, FOwnerDataLayerRequests>& Pair : RequestsByOwner)
	{
		if (!Pair.Value.IsEmpty())
		{
			ResolveDataLayerMappingForOwner(InOutSnapshot.DataLayerOwnerCaches.FindChecked(Pair.Key), EGitChangedAssetDataLayerMappingSource::Current, Pair.Value, bForceRebuild);
			ResolveDataLayerMappingForOwner(InOutSnapshot.DataLayerOwnerCaches.FindChecked(Pair.Key), EGitChangedAssetDataLayerMappingSource::Head, Pair.Value, bForceRebuild);
		}
	}
	RebuildCachedDataLayerDisplays(InOutSnapshot);
}

void FinalizeRevertEligibility(FGitChangedAssetEntry& InOutEntry)
{
	if (!InOutEntry.bBaseRevertEligible)
	{
		return;
	}
	if ((InOutEntry.PackageKind == EGitChangedAssetPackageKind::ExternalActor || InOutEntry.PackageKind == EGitChangedAssetPackageKind::ExternalObject) && !InOutEntry.bOwnerLevelResolved)
	{
		InOutEntry.bCanRevert = false;
		InOutEntry.RevertBlockReason = TEXT("无法唯一确定 OFPA 资产所属的关卡, 因此不能安全回退。");
		return;
	}
	InOutEntry.bCanRevert = true;
	InOutEntry.RevertBlockReason.Reset();
}

bool LoadMetadataFromBlob(IAssetRegistry& InAssetRegistry, const TArray<uint8>& InBlobData, const FString& InLocalLfsObjectFilename,
	const FString& InLogicalFilename, TArray<FAssetData>& OutAssetData, FString& OutFailureReason)
{
	check(IsInGameThread());
	FGitLfsPointer LfsPointer;
	const EGitLfsPointerParseResult PointerResult = ParseGitLfsPointer(InBlobData, LfsPointer);
	if (PointerResult == EGitLfsPointerParseResult::InvalidPointer)
	{
		OutFailureReason = TEXT("HEAD Git blob 包含无效的 Git LFS pointer。");
		return false;
	}
	if (PointerResult == EGitLfsPointerParseResult::ValidPointer)
	{
		if (InLocalLfsObjectFilename.IsEmpty() || IFileManager::Get().FileSize(*InLocalLfsObjectFilename) != LfsPointer.Size)
		{
			OutFailureReason = TEXT("本地 Git LFS object 不可用; 列表不会自动下载 metadata。");
			return false;
		}
		TUniquePtr<FArchive> FileReader(IFileManager::Get().CreateFileReader(*InLocalLfsObjectFilename));
		if (!FileReader)
		{
			OutFailureReason = TEXT("无法打开本地 Git LFS object。");
			return false;
		}
		FLogicalFilenameArchiveProxy LogicalReader(*FileReader, InLogicalFilename);
		IAssetRegistry::FLoadPackageRegistryData PackageData;
		InAssetRegistry.LoadPackageRegistryData(LogicalReader, PackageData);
		if (FileReader->IsError())
		{
			OutFailureReason = TEXT("无法从本地 Git LFS object 读取 package metadata。");
			return false;
		}
		OutAssetData = MoveTemp(PackageData.Data);
	}
	else
	{
		if (InBlobData.IsEmpty())
		{
			OutFailureReason = TEXT("HEAD Git blob 不包含可读取的 package metadata。");
			return false;
		}
		FLargeMemoryReader Reader(InBlobData.GetData(), InBlobData.Num(), ELargeMemoryReaderFlags::None, FName(*InLogicalFilename));
		IAssetRegistry::FLoadPackageRegistryData PackageData;
		InAssetRegistry.LoadPackageRegistryData(Reader, PackageData);
		if (Reader.IsError())
		{
			OutFailureReason = TEXT("无法从 HEAD package header 读取 Asset Registry metadata。");
			return false;
		}
		OutAssetData = MoveTemp(PackageData.Data);
	}
	if (OutAssetData.IsEmpty())
	{
		OutFailureReason = TEXT("package header 不含 Asset Registry metadata。");
		return false;
	}
	return true;
}

bool ApplyCurrentPackageHeaderMetadata(IAssetRegistry& InAssetRegistry, const FString& InFilename,
	FGitChangedAssetEntry& InOutEntry, FString& OutFailureReason)
{
	OutFailureReason.Reset();
	IAssetRegistry::FLoadPackageRegistryData PackageData;
	InAssetRegistry.LoadPackageRegistryData(InFilename, PackageData);
	if (PackageData.Data.IsEmpty())
	{
		OutFailureReason = TEXT("package header 不含 Asset Registry metadata。");
		return false;
	}
	ApplyAssetData(InOutEntry, PackageData.Data[0], EGitChangedAssetMetadataSource::CurrentAssetRegistry, true);
	return true;
}

bool IsWorldDataLayersAssetData(const FAssetData& InAssetData);
void SeedHeadOnlyWorldDataLayersIndex(FGitChangedAssetSnapshot& InOutSnapshot, const FString& InOwnerLevel,
	const FString& InHeadRepositoryRelativePath, EGitChangedAssetState InState, const FAssetData& InAssetData);
bool TryResolveHeadWorldDataLayersOwner(const FGitChangedAssetSnapshot& InSnapshot, const FAssetData& InAssetData,
	const FString& InHeadRepositoryRelativePath, const FGitChangedAssetEntry* InFallbackEntry, FString& OutOwnerLevel);

void ApplyHeadWorldDataLayersMetadata(FGitChangedAssetSnapshot& InOutSnapshot, const FGitChangedAssetHeadMetadataResult& InResult,
	const TArray<FAssetData>& InAssetData, const FString& InFailureReason)
{
	FGitChangedAssetDataLayerOwnerCache* OwnerCache = InOutSnapshot.DataLayerOwnerCaches.Find(InResult.DataLayerOwnerLevel);
	if (!OwnerCache || InResult.WorldDataLayersRepositoryRelativePath.IsEmpty())
	{
		return;
	}

	FGitChangedAssetDataLayerMappingCache& HeadCache = OwnerCache->Head;
	HeadCache.AttemptedWorldDataLayersRepositoryPaths.Add(InResult.WorldDataLayersRepositoryRelativePath);
	if (InResult.WorldDataLayersState == EGitChangedAssetState::Added || InResult.WorldDataLayersState == EGitChangedAssetState::Untracked)
	{
		HeadCache.KnownAbsentWorldDataLayersRepositoryPaths.Add(InResult.WorldDataLayersRepositoryRelativePath);
		HeadCache.UnavailableWorldDataLayersRepositoryPaths.Remove(InResult.WorldDataLayersRepositoryRelativePath);
		return;
	}
	if (!InFailureReason.IsEmpty() || InAssetData.IsEmpty())
	{
		HeadCache.UnavailableWorldDataLayersRepositoryPaths.Add(InResult.WorldDataLayersRepositoryRelativePath);
		return;
	}
	FString HeadOwnerLevel;
	if (!IsWorldDataLayersAssetData(InAssetData[0]) ||
		!TryResolveHeadWorldDataLayersOwner(InOutSnapshot, InAssetData[0], InResult.WorldDataLayersRepositoryRelativePath, nullptr, HeadOwnerLevel))
	{
		HeadCache.UnavailableWorldDataLayersRepositoryPaths.Add(InResult.WorldDataLayersRepositoryRelativePath);
		HeadCache.KnownAbsentWorldDataLayersRepositoryPaths.Remove(InResult.WorldDataLayersRepositoryRelativePath);
		HeadCache.HeadWorldDataLayersAssetData.Remove(InResult.WorldDataLayersRepositoryRelativePath);
		return;
	}
	if (!HeadOwnerLevel.Equals(InResult.DataLayerOwnerLevel, ESearchCase::CaseSensitive))
	{
		for (FGitChangedAssetWorldDataLayersIndexEntry& IndexEntry : OwnerCache->WorldDataLayers)
		{
			if (IndexEntry.RepositoryRelativePath.Equals(InResult.WorldDataLayersRepositoryRelativePath, ESearchCase::CaseSensitive))
			{
				IndexEntry.bKnownAbsentAtHead = true;
			}
		}
		HeadCache.KnownAbsentWorldDataLayersRepositoryPaths.Add(InResult.WorldDataLayersRepositoryRelativePath);
		HeadCache.UnavailableWorldDataLayersRepositoryPaths.Remove(InResult.WorldDataLayersRepositoryRelativePath);
		HeadCache.HeadWorldDataLayersAssetData.Remove(InResult.WorldDataLayersRepositoryRelativePath);
		SeedHeadOnlyWorldDataLayersIndex(InOutSnapshot, HeadOwnerLevel, InResult.WorldDataLayersRepositoryRelativePath,
			InResult.WorldDataLayersState, InAssetData[0]);
		return;
	}
	HeadCache.UnavailableWorldDataLayersRepositoryPaths.Remove(InResult.WorldDataLayersRepositoryRelativePath);
	HeadCache.HeadWorldDataLayersAssetData.Add(InResult.WorldDataLayersRepositoryRelativePath, InAssetData[0]);
}

bool IsWorldDataLayersAssetData(const FAssetData& InAssetData)
{
	if (!FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(InAssetData))
	{
		return false;
	}
	FString ActorDescriptorFailureReason;
	TUniquePtr<FWorldPartitionActorDesc> ActorDesc = TryCreateActorDescriptorFromAssetData(InAssetData, ActorDescriptorFailureReason);
	return ActorDesc && TryGetWorldDataLayersActorDesc(*ActorDesc) != nullptr;
}

bool TryDeriveHeadOwnerFromRepositoryRelativePath(const FGitChangedAssetSnapshot& InSnapshot,
	const FString& InRepositoryRelativePath, FString& OutOwnerLevel)
{
	if (!IsGitChangedAssetUassetPath(InRepositoryRelativePath) || InSnapshot.RepositoryRoot.IsEmpty())
	{
		return false;
	}
	FGitChangedAssetEntry Entry;
	Entry.RepositoryRelativePath = InRepositoryRelativePath;
	Entry.AbsoluteFilename = FPaths::Combine(InSnapshot.RepositoryRoot, InRepositoryRelativePath);
	EnsurePackageName(Entry);
	if (!TryDeriveStandardExternalOwnerLevel(Entry))
	{
		return false;
	}
	OutOwnerLevel = MoveTemp(Entry.OwnerLevel);
	return true;
}

bool TryResolveHeadWorldDataLayersOwner(const FGitChangedAssetSnapshot& InSnapshot, const FAssetData& InAssetData,
	const FString& InHeadRepositoryRelativePath, const FGitChangedAssetEntry* InFallbackEntry, FString& OutOwnerLevel)
{
	OutOwnerLevel.Reset();
	if (TryGetOwnerLevelFromOuterPath(InAssetData, OutOwnerLevel))
	{
		return true;
	}

	auto TryResolveActorSoftPathOwner = [&InAssetData, &OutOwnerLevel]()
	{
		FString ActorDescriptorFailureReason;
		if (TUniquePtr<FWorldPartitionActorDesc> ActorDesc = TryCreateActorDescriptorFromAssetData(InAssetData, ActorDescriptorFailureReason))
		{
			if (TryGetWorldDataLayersActorDesc(*ActorDesc))
			{
				const FTopLevelAssetPath ActorAssetPath = ActorDesc->GetActorSoftPath().GetAssetPath();
				const FString ActorOwnerLevel = ActorAssetPath.GetPackageName().ToString();
				if (FPackageName::IsValidLongPackageName(ActorOwnerLevel, true))
				{
					OutOwnerLevel = ActorOwnerLevel;
					return true;
				}
			}
		}
		return false;
	};

	if (InFallbackEntry && InFallbackEntry->IsRename())
	{
		if (TryResolveActorSoftPathOwner() || TryDeriveHeadOwnerFromRepositoryRelativePath(InSnapshot, InHeadRepositoryRelativePath, OutOwnerLevel) ||
			TryDeriveHeadOwnerFromRepositoryRelativePath(InSnapshot, InFallbackEntry->RenameFromRepositoryRelativePath, OutOwnerLevel))
		{
			return true;
		}
		// A current-owner fallback would leak renamed WDL metadata across maps. The
		// unknown state is intentionally preserved until a unique HEAD owner exists.
		return false;
	}

	if (InFallbackEntry && InFallbackEntry->bOwnerLevelResolved && !InFallbackEntry->OwnerLevel.IsEmpty())
	{
		OutOwnerLevel = InFallbackEntry->OwnerLevel;
		return true;
	}

	return TryResolveActorSoftPathOwner() || TryDeriveHeadOwnerFromRepositoryRelativePath(InSnapshot, InHeadRepositoryRelativePath, OutOwnerLevel);
}

void SeedHeadOnlyWorldDataLayersIndex(FGitChangedAssetSnapshot& InOutSnapshot, const FString& InOwnerLevel,
	const FString& InHeadRepositoryRelativePath, const EGitChangedAssetState InState, const FAssetData& InAssetData)
{
	if (InOwnerLevel.IsEmpty() || !IsGitChangedAssetUassetPath(InHeadRepositoryRelativePath))
	{
		return;
	}
	FGitChangedAssetDataLayerOwnerCache& OwnerCache = InOutSnapshot.DataLayerOwnerCaches.FindOrAdd(InOwnerLevel);
	FGitChangedAssetWorldDataLayersIndexEntry* IndexEntry = OwnerCache.WorldDataLayers.FindByPredicate(
		[&InHeadRepositoryRelativePath](const FGitChangedAssetWorldDataLayersIndexEntry& Candidate)
		{
			return Candidate.RepositoryRelativePath.Equals(InHeadRepositoryRelativePath, ESearchCase::CaseSensitive);
		});
	if (!IndexEntry)
	{
		IndexEntry = &OwnerCache.WorldDataLayers.AddDefaulted_GetRef();
		IndexEntry->RepositoryRelativePath = InHeadRepositoryRelativePath;
		IndexEntry->AssetData = InAssetData;
	}
	IndexEntry->State = InState;
	IndexEntry->bChangedRelativeToHead = true;

	FGitChangedAssetDataLayerMappingCache& HeadCache = OwnerCache.Head;
	HeadCache.AttemptedWorldDataLayersRepositoryPaths.Add(InHeadRepositoryRelativePath);
	if (InState == EGitChangedAssetState::Added || InState == EGitChangedAssetState::Untracked)
	{
		HeadCache.KnownAbsentWorldDataLayersRepositoryPaths.Add(InHeadRepositoryRelativePath);
		HeadCache.UnavailableWorldDataLayersRepositoryPaths.Remove(InHeadRepositoryRelativePath);
		HeadCache.HeadWorldDataLayersAssetData.Remove(InHeadRepositoryRelativePath);
		return;
	}
	HeadCache.UnavailableWorldDataLayersRepositoryPaths.Remove(InHeadRepositoryRelativePath);
	HeadCache.HeadWorldDataLayersAssetData.Add(InHeadRepositoryRelativePath, InAssetData);
}

void SeedHeadOnlyWorldDataLayersIndexFromChangedAsset(FGitChangedAssetSnapshot& InOutSnapshot,
	const FGitChangedAssetEntry& InEntry, const FGitChangedAssetHeadMetadataResult& InHeadMetadata,
	const TArray<FAssetData>& InAssetData, const FString& InFailureReason)
{
	if (!InFailureReason.IsEmpty())
	{
		return;
	}
	const FString HeadRepositoryRelativePath = InEntry.IsRename() && IsGitChangedAssetUassetPath(InEntry.RenameFromRepositoryRelativePath)
		? InEntry.RenameFromRepositoryRelativePath
		: InEntry.RepositoryRelativePath;
	if (!IsGitChangedAssetUassetPath(HeadRepositoryRelativePath))
	{
		return;
	}
	for (const FAssetData& AssetData : InAssetData)
	{
		if (!IsWorldDataLayersAssetData(AssetData))
		{
			continue;
		}
		FString OwnerLevel;
		if (TryResolveHeadWorldDataLayersOwner(InOutSnapshot, AssetData, HeadRepositoryRelativePath, &InEntry, OwnerLevel))
		{
			SeedHeadOnlyWorldDataLayersIndex(InOutSnapshot, OwnerLevel, HeadRepositoryRelativePath, InEntry.State, AssetData);
		}
		return;
	}
}

bool ShouldResolveCurrentFileMetadata(const FGitChangedAssetEntry& InEntry)
{
	// A staged deletion can coexist with an untracked replacement at the same filename.
	// Its current bytes are never the deleted asset's metadata source.
	return InEntry.State != EGitChangedAssetState::Deleted;
}

bool GetHeadMetadataSourcePath(const FGitChangedAssetEntry& InEntry, const bool bCurrentFilenameExists,
	FString& OutRepositoryRelativePath, FString& OutLogicalFilename)
{
	OutRepositoryRelativePath.Reset();
	OutLogicalFilename.Reset();
	if (InEntry.State == EGitChangedAssetState::Deleted)
	{
		if (!IsGitChangedAssetUassetPath(InEntry.RepositoryRelativePath))
		{
			return false;
		}
		OutRepositoryRelativePath = InEntry.RepositoryRelativePath;
		OutLogicalFilename = InEntry.AbsoluteFilename;
		return true;
	}
	if (InEntry.State == EGitChangedAssetState::Renamed && !bCurrentFilenameExists &&
		IsGitChangedAssetUassetPath(InEntry.RenameFromRepositoryRelativePath))
	{
		OutRepositoryRelativePath = InEntry.RenameFromRepositoryRelativePath;
		OutLogicalFilename = InEntry.RenameFromAbsoluteFilename;
		return true;
	}
	if (!bCurrentFilenameExists && InEntry.State != EGitChangedAssetState::Added && InEntry.State != EGitChangedAssetState::Untracked &&
		IsGitChangedAssetUassetPath(InEntry.RepositoryRelativePath))
	{
		OutRepositoryRelativePath = InEntry.RepositoryRelativePath;
		OutLogicalFilename = InEntry.AbsoluteFilename;
		return true;
	}
	return false;
}
}

void FGitChangedAssetsMetadataResolver::ResolveCurrentMetadata(FGitChangedAssetSnapshot& InOutSnapshot)
{
	using namespace GitChangedAssetsMetadataPrivate;
	check(IsInGameThread());

	TArray<FName> PackageNames;
	TMap<FName, TArray<FGitChangedAssetEntry*>> EntriesByPackage;
	TArray<FGitChangedAssetEntry*> ExistingEntries;
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		EnsureFallbackDisplay(Entry);
		if (ShouldResolveCurrentFileMetadata(Entry) && FPaths::FileExists(Entry.AbsoluteFilename))
		{
			ExistingEntries.Add(&Entry);
			if (!Entry.PackageName.IsEmpty())
			{
				const FName PackageName(*Entry.PackageName);
				PackageNames.AddUnique(PackageName);
				EntriesByPackage.FindOrAdd(PackageName).Add(&Entry);
			}
		}
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	TMap<FName, FAssetData> AssetDataByPackage;
	auto QueryPackages = [&AssetRegistry, &AssetDataByPackage](const TArray<FName>& InPackageNames)
	{
		if (InPackageNames.IsEmpty())
		{
			return;
		}
		FARFilter Filter;
		Filter.PackageNames = InPackageNames;
		Filter.bIncludeOnlyOnDiskAssets = true;
		TArray<FAssetData> AssetData;
		AssetRegistry.GetAssets(Filter, AssetData, false);
		for (const FAssetData& Data : AssetData)
		{
			AssetDataByPackage.FindOrAdd(Data.PackageName) = Data;
		}
	};
	QueryPackages(PackageNames);

	TArray<FString> FilesToScan;
	TArray<FName> MissedPackageNames;
	for (FGitChangedAssetEntry* Entry : ExistingEntries)
	{
		if (Entry->PackageName.IsEmpty() || !AssetDataByPackage.Contains(FName(*Entry->PackageName)))
		{
			FilesToScan.AddUnique(Entry->AbsoluteFilename);
			if (!Entry->PackageName.IsEmpty())
			{
				MissedPackageNames.AddUnique(FName(*Entry->PackageName));
			}
		}
	}
	if (!FilesToScan.IsEmpty())
	{
		AssetRegistry.ScanModifiedAssetFiles(FilesToScan);
		QueryPackages(MissedPackageNames);
	}

	for (const TPair<FName, TArray<FGitChangedAssetEntry*>>& Pair : EntriesByPackage)
	{
		if (const FAssetData* Data = AssetDataByPackage.Find(Pair.Key))
		{
			for (FGitChangedAssetEntry* Entry : Pair.Value)
			{
				ApplyAssetData(*Entry, *Data, EGitChangedAssetMetadataSource::CurrentAssetRegistry, true);
			}
		}
	}

	for (FGitChangedAssetEntry* Entry : ExistingEntries)
	{
		const bool bResolvedByAssetRegistry = !Entry->PackageName.IsEmpty() && AssetDataByPackage.Contains(FName(*Entry->PackageName));
		if (bResolvedByAssetRegistry)
		{
			continue;
		}

		FString HeaderFailureReason;
		if (!ApplyCurrentPackageHeaderMetadata(AssetRegistry, Entry->AbsoluteFilename, *Entry, HeaderFailureReason))
		{
			SetMetadataFailure(*Entry, FString::Printf(TEXT("当前 .uasset 无法从 Asset Registry 或 package header 读取 metadata: %s"), *HeaderFailureReason));
		}
	}

	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		// This derives only standard external package topology and does not load a map.
		// Resolve it for deleted entries as well so the fixed-HEAD batch can include a
		// changed owner WorldDataLayers descriptor before HEAD actor metadata is applied.
		if (!Entry.bOwnerLevelResolved)
		{
			TryDeriveStandardExternalOwnerLevel(Entry);
		}
	}
	ResolvePrivateAndDeprecatedDataLayerNames(InOutSnapshot);
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		FinalizeRevertEligibility(Entry);
	}
}

bool FGitChangedAssetsMetadataResolver::ResolveHeadOnlyMetadata(const FGitChangedAssetSnapshot& InSnapshot,
	TArray<FGitChangedAssetHeadMetadataResult>& OutResults, FString& OutError,
	TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> InCancellationContext)
{
	using namespace GitChangedAssetsMetadataPrivate;
	GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(InCancellationContext);

	OutResults.Reset();
	OutError.Reset();
	if (InCancellationContext.IsValid() && InCancellationContext->IsCancellationRequested())
	{
		OutError = TEXT("Changed Assets HEAD metadata read was cancelled.");
		return false;
	}
	if (InSnapshot.PinnedHead.IsEmpty() || InSnapshot.GitBinary.IsEmpty() || InSnapshot.RepositoryRoot.IsEmpty())
	{
		OutError = TEXT("Changed Assets HEAD metadata 缺少固定 Git snapshot。" );
		return false;
	}

	struct FHeadMetadataRequestTarget
	{
		int32 EntryIndex = INDEX_NONE;
		FString LogicalFilename;
		FString DataLayerOwnerLevel;
		FString WorldDataLayersRepositoryRelativePath;
		EGitChangedAssetState WorldDataLayersState = EGitChangedAssetState::Modified;
	};
	TArray<FGitCatFileBatchRequest> Requests;
	TArray<FHeadMetadataRequestTarget> RequestTargets;
	for (int32 EntryIndex = 0; EntryIndex < InSnapshot.Entries.Num(); ++EntryIndex)
	{
		const FGitChangedAssetEntry& Entry = InSnapshot.Entries[EntryIndex];
		FString HeadRepositoryRelativePath;
		FString LogicalFilename;
		if (GetHeadMetadataSourcePath(Entry, FPaths::FileExists(Entry.AbsoluteFilename), HeadRepositoryRelativePath, LogicalFilename))
		{
			FGitCatFileBatchRequest& Request = Requests.AddDefaulted_GetRef();
			Request.ObjectSpec = FString::Printf(TEXT("%s:%s"), *InSnapshot.PinnedHead, *HeadRepositoryRelativePath);
			FHeadMetadataRequestTarget& Target = RequestTargets.AddDefaulted_GetRef();
			Target.EntryIndex = EntryIndex;
			Target.LogicalFilename = MoveTemp(LogicalFilename);
		}
	}
	for (const TPair<FString, FGitChangedAssetDataLayerOwnerCache>& Pair : InSnapshot.DataLayerOwnerCaches)
	{
		for (const FGitChangedAssetWorldDataLayersIndexEntry& WorldDataLayers : Pair.Value.WorldDataLayers)
		{
			if (!WorldDataLayers.bChangedRelativeToHead || WorldDataLayers.State == EGitChangedAssetState::Added ||
				WorldDataLayers.State == EGitChangedAssetState::Untracked || !IsGitChangedAssetUassetPath(WorldDataLayers.RepositoryRelativePath))
			{
				continue;
			}
			FGitCatFileBatchRequest& Request = Requests.AddDefaulted_GetRef();
			Request.ObjectSpec = FString::Printf(TEXT("%s:%s"), *InSnapshot.PinnedHead, *WorldDataLayers.RepositoryRelativePath);
			FHeadMetadataRequestTarget& Target = RequestTargets.AddDefaulted_GetRef();
			Target.DataLayerOwnerLevel = Pair.Key;
			Target.WorldDataLayersRepositoryRelativePath = WorldDataLayers.RepositoryRelativePath;
			Target.WorldDataLayersState = WorldDataLayers.State;
			Target.LogicalFilename = FPaths::ConvertRelativePathToFull(InSnapshot.RepositoryRoot, WorldDataLayers.RepositoryRelativePath);
			FPaths::NormalizeFilename(Target.LogicalFilename);
		}
	}
	if (Requests.IsEmpty())
	{
		return true;
	}

	TArray<FGitCatFileBatchResult> Results;
	if (!FGitCatFileBatchReader::ReadBlobs(InSnapshot.GitBinary, InSnapshot.RepositoryRoot, Requests, Results, OutError,
		HeadMetadataBlobLimitBytes, HeadMetadataTotalBlobLimitBytes, 30.0, InCancellationContext))
	{
		return false;
	}
	if (Results.Num() != RequestTargets.Num())
	{
		OutError = TEXT("git cat-file 返回的 metadata result 数量不匹配。" );
		return false;
	}

	FGitLfsLocalObjectStore LfsObjectStore(InSnapshot.GitBinary, InSnapshot.RepositoryRoot);
	for (int32 ResultIndex = 0; ResultIndex < Results.Num(); ++ResultIndex)
	{
		if (InCancellationContext.IsValid() && InCancellationContext->IsCancellationRequested())
		{
			OutError = TEXT("Changed Assets HEAD metadata read was cancelled.");
			return false;
		}
		FGitChangedAssetHeadMetadataResult& HeadMetadata = OutResults.AddDefaulted_GetRef();
		const FHeadMetadataRequestTarget& Target = RequestTargets[ResultIndex];
		HeadMetadata.EntryIndex = Target.EntryIndex;
		HeadMetadata.DataLayerOwnerLevel = Target.DataLayerOwnerLevel;
		HeadMetadata.WorldDataLayersRepositoryRelativePath = Target.WorldDataLayersRepositoryRelativePath;
		HeadMetadata.WorldDataLayersState = Target.WorldDataLayersState;
		HeadMetadata.LogicalFilename = Target.LogicalFilename;
		FGitCatFileBatchResult& Result = Results[ResultIndex];
		if (!Result.bFound || Result.bSkippedBySizeLimit)
		{
			HeadMetadata.FailureReason = Result.Error.IsEmpty() ? TEXT("HEAD metadata 不可用。") : Result.Error;
			continue;
		}
		if (HeadMetadata.LogicalFilename.IsEmpty())
		{
			HeadMetadata.FailureReason = TEXT("HEAD metadata 不含逻辑 .uasset 文件名。");
			continue;
		}
		HeadMetadata.BlobData = MoveTemp(Result.Data);
		FGitLfsPointer LfsPointer;
		const EGitLfsPointerParseResult PointerResult = ParseGitLfsPointer(HeadMetadata.BlobData, LfsPointer);
		if (PointerResult == EGitLfsPointerParseResult::InvalidPointer)
		{
			HeadMetadata.FailureReason = TEXT("HEAD Git blob 包含无效的 Git LFS pointer。");
			continue;
		}
		if (PointerResult == EGitLfsPointerParseResult::ValidPointer)
		{
			FString LookupError;
			const EGitLfsLocalObjectLookupResult LookupResult = LfsObjectStore.FindObject(LfsPointer, HeadMetadata.LocalLfsObjectFilename, LookupError);
			if (LookupResult != EGitLfsLocalObjectLookupResult::Found)
			{
				HeadMetadata.LocalLfsObjectFilename.Reset();
				HeadMetadata.FailureReason = LookupError.IsEmpty()
					? TEXT("本地 Git LFS object 不可用; 列表不会自动下载 metadata。")
					: LookupError;
			}
		}
	}
	return true;
}

int32 FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadataRange(FGitChangedAssetSnapshot& InOutSnapshot,
	const TArray<FGitChangedAssetHeadMetadataResult>& InResults, const int32 InStartIndex, const int32 InMaxCount)
{
	using namespace GitChangedAssetsMetadataPrivate;
	check(IsInGameThread());
	if (InMaxCount <= 0 || InStartIndex < 0 || InStartIndex >= InResults.Num())
	{
		return 0;
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	const int32 EndIndex = FMath::Min(InStartIndex + InMaxCount, InResults.Num());
	for (int32 ResultIndex = InStartIndex; ResultIndex < EndIndex; ++ResultIndex)
	{
		const FGitChangedAssetHeadMetadataResult& HeadMetadata = InResults[ResultIndex];
		TArray<FAssetData> AssetData;
		FString FailureReason = HeadMetadata.FailureReason;
		if (FailureReason.IsEmpty() && !LoadMetadataFromBlob(AssetRegistry, HeadMetadata.BlobData, HeadMetadata.LocalLfsObjectFilename,
			HeadMetadata.LogicalFilename, AssetData, FailureReason))
		{
			// The result remains source-local: a missing LFS object or malformed header
			// cannot be substituted with current worktree metadata.
		}
		if (HeadMetadata.IsWorldDataLayersMetadata())
		{
			ApplyHeadWorldDataLayersMetadata(InOutSnapshot, HeadMetadata, AssetData, FailureReason);
			continue;
		}
		if (!InOutSnapshot.Entries.IsValidIndex(HeadMetadata.EntryIndex))
		{
			continue;
		}
		FGitChangedAssetEntry& Entry = InOutSnapshot.Entries[HeadMetadata.EntryIndex];
		SeedHeadOnlyWorldDataLayersIndexFromChangedAsset(InOutSnapshot, Entry, HeadMetadata, AssetData, FailureReason);
		if (!FailureReason.IsEmpty())
		{
			SetMetadataFailure(Entry, FailureReason);
			continue;
		}
		if (AssetData.IsEmpty())
		{
			SetMetadataFailure(Entry, TEXT("HEAD package header 不含 Asset Registry metadata。"));
			continue;
		}
		ApplyAssetData(Entry, AssetData[0], EGitChangedAssetMetadataSource::HeadPackageRegistry, true);
	}
	return EndIndex - InStartIndex;
}

void FGitChangedAssetsMetadataResolver::FinalizeHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot)
{
	using namespace GitChangedAssetsMetadataPrivate;
	check(IsInGameThread());
	ResolvePrivateAndDeprecatedDataLayerNames(InOutSnapshot, true);
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		FinalizeRevertEligibility(Entry);
	}
}

void FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot,
	const TArray<FGitChangedAssetHeadMetadataResult>& InResults)
{
	ApplyHeadOnlyMetadataRange(InOutSnapshot, InResults, 0, InResults.Num());
	FinalizeHeadOnlyMetadata(InOutSnapshot);
}

void FGitChangedAssetsMetadataResolver::ResolveOutstandingOwnerFallback(FGitChangedAssetSnapshot& InOutSnapshot)
{
	using namespace GitChangedAssetsMetadataPrivate;
	check(IsInGameThread());

	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		if (!Entry.bOwnerLevelResolved)
		{
		TryDeriveStandardExternalOwnerLevel(Entry);
		}
	}
	ResolveExternalOwnersByWorldRoots(InOutSnapshot);
	ResolvePrivateAndDeprecatedDataLayerNames(InOutSnapshot);
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		FinalizeRevertEligibility(Entry);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
bool GitChangedAssetsMetadataTesting::ApplyPackageHeaderMetadata(const FString& InFilename, FGitChangedAssetEntry& InOutEntry, FString& OutFailureReason)
{
	check(IsInGameThread());
	GitChangedAssetsMetadataPrivate::EnsureFallbackDisplay(InOutEntry);
	return GitChangedAssetsMetadataPrivate::ApplyCurrentPackageHeaderMetadata(IAssetRegistry::GetChecked(), InFilename, InOutEntry, OutFailureReason);
}

bool GitChangedAssetsMetadataTesting::EnsurePackageName(FGitChangedAssetEntry& InOutEntry)
{
	return GitChangedAssetsMetadataPrivate::EnsurePackageName(InOutEntry);
}

bool GitChangedAssetsMetadataTesting::GetHeadMetadataSourcePath(const FGitChangedAssetEntry& InEntry, const bool bCurrentFilenameExists,
	FString& OutRepositoryRelativePath, FString& OutLogicalFilename)
{
	return GitChangedAssetsMetadataPrivate::GetHeadMetadataSourcePath(InEntry, bCurrentFilenameExists,
		OutRepositoryRelativePath, OutLogicalFilename);
}

bool GitChangedAssetsMetadataTesting::ShouldResolveCurrentFileMetadata(const FGitChangedAssetEntry& InEntry)
{
	return GitChangedAssetsMetadataPrivate::ShouldResolveCurrentFileMetadata(InEntry);
}

bool GitChangedAssetsMetadataTesting::LoadHeadMetadataBlob(const FString& InGitBinary, const FString& InRepositoryRoot, const FString& InLogicalFilename,
	const TArray<uint8>& InBlobData, TArray<FAssetData>& OutAssetData, FString& OutFailureReason)
{
	check(IsInGameThread());
	FGitLfsPointer Pointer;
	FString LocalLfsObjectFilename;
	const EGitLfsPointerParseResult PointerResult = ParseGitLfsPointer(InBlobData, Pointer);
	if (PointerResult == EGitLfsPointerParseResult::InvalidPointer)
	{
		OutFailureReason = TEXT("HEAD Git blob 包含无效的 Git LFS pointer。");
		return false;
	}
	if (PointerResult == EGitLfsPointerParseResult::ValidPointer)
	{
		FGitLfsLocalObjectStore LfsObjectStore(InGitBinary, InRepositoryRoot);
		if (LfsObjectStore.FindObject(Pointer, LocalLfsObjectFilename, OutFailureReason) != EGitLfsLocalObjectLookupResult::Found)
		{
			if (OutFailureReason.IsEmpty())
			{
				OutFailureReason = TEXT("本地 Git LFS object 不可用; 列表不会自动下载 metadata。");
			}
			return false;
		}
	}
	return GitChangedAssetsMetadataPrivate::LoadMetadataFromBlob(IAssetRegistry::GetChecked(), InBlobData, LocalLfsObjectFilename,
		InLogicalFilename, OutAssetData, OutFailureReason);
}

void GitChangedAssetsMetadataTesting::ApplyAssetData(const FAssetData& InAssetData, FGitChangedAssetEntry& InOutEntry)
{
	check(IsInGameThread());
	GitChangedAssetsMetadataPrivate::ApplyAssetData(InOutEntry, InAssetData, EGitChangedAssetMetadataSource::CurrentAssetRegistry, true);
}

FString GitChangedAssetsMetadataTesting::FriendlyDataLayerName(const FString& InDataLayerPath)
{
	return GitChangedAssetsMetadataPrivate::FriendlyDataLayerName(InDataLayerPath);
}

FString GitChangedAssetsMetadataTesting::BuildActorDisplayObjectPath(const FString& InObjectPath, const FString& InActorName,
	const TArray<FString>& InFriendlyDataLayerNames)
{
	return GitChangedAssetsMetadataPrivate::BuildActorDisplayObjectPath(InObjectPath, InActorName, InFriendlyDataLayerNames);
}

bool GitChangedAssetsMetadataTesting::RequiresWorldDataLayersDescriptor(const FName InDataLayerIdentifier)
{
	return GitChangedAssetsMetadataPrivate::RequiresWorldDataLayersDescriptor(InDataLayerIdentifier);
}

void GitChangedAssetsMetadataTesting::ApplyResolvedDataLayerNames(FGitChangedAssetEntry& InOutEntry, const TMap<FName, FString>& InResolvedNames)
{
	GitChangedAssetsMetadataPrivate::RebuildActorDataLayerDisplay(InOutEntry, &InResolvedNames);
}

void GitChangedAssetsMetadataTesting::ApplySourceSpecificDataLayerNames(FGitChangedAssetEntry& InOutEntry,
	const TMap<FName, FString>& InCurrentNames, const TMap<FName, FString>& InHeadNames)
{
	const TMap<FName, FString>& Names = InOutEntry.DataLayerMappingSource == EGitChangedAssetDataLayerMappingSource::Head
		? InHeadNames
		: InCurrentNames;
	GitChangedAssetsMetadataPrivate::RebuildActorDataLayerDisplay(InOutEntry, &Names);
}

void GitChangedAssetsMetadataTesting::SeedHeadOnlyWorldDataLayersIndex(FGitChangedAssetSnapshot& InOutSnapshot, const FString& InOwnerLevel,
	const FString& InHeadRepositoryRelativePath, const EGitChangedAssetState InState)
{
	GitChangedAssetsMetadataPrivate::SeedHeadOnlyWorldDataLayersIndex(InOutSnapshot, InOwnerLevel, InHeadRepositoryRelativePath, InState, FAssetData());
}

bool GitChangedAssetsMetadataTesting::HasCompleteHeadWorldDataLayersMetadata(const FGitChangedAssetDataLayerOwnerCache& InOwnerCache)
{
	return GitChangedAssetsMetadataPrivate::HasCompleteHeadWorldDataLayersMetadata(InOwnerCache);
}

bool GitChangedAssetsMetadataTesting::ResolveHeadWorldDataLayersOwnerWithoutOuter(const FGitChangedAssetSnapshot& InSnapshot,
	const FGitChangedAssetEntry& InFallbackEntry, FString& OutOwnerLevel)
{
	return GitChangedAssetsMetadataPrivate::TryResolveHeadWorldDataLayersOwner(InSnapshot, FAssetData(),
		InFallbackEntry.RepositoryRelativePath, &InFallbackEntry, OutOwnerLevel);
}

bool GitChangedAssetsMetadataTesting::IsUsableWorldDataLayersDescriptor(const FWorldPartitionActorDesc& InActorDesc)
{
	return GitChangedAssetsMetadataPrivate::TryGetWorldDataLayersActorDesc(InActorDesc) != nullptr;
}
#endif
