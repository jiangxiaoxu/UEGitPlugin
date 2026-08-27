// Copyright (c) 2026
//
// Changed Assets 的 Asset Registry metadata 解析. 不加载 UObject, 不启动网络 Git/LFS 操作.

#include "GitChangedAssetsMetadata.h"

#include "GitCatFileBatchReader.h"
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
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/LargeMemoryReader.h"
#include "UObject/PrimaryAssetId.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"

namespace GitChangedAssetsMetadataPrivate
{
constexpr int64 HeadMetadataBlobLimitBytes = 1024 * 1024;
constexpr int64 HeadMetadataTotalBlobLimitBytes = 8 * 1024 * 1024;

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

bool EnsurePackageName(FGitChangedAssetEntry& InOutEntry)
{
	if (!InOutEntry.AbsoluteFilename.IsEmpty())
	{
		FString CanonicalLocalPathNoExtension;
		FString MountedPackageName;
		FString Extension;
		FPackageName::EErrorCode MountedPathFailureReason = FPackageName::EErrorCode::PackageNameUnknown;
		if (FPackageName::TryConvertToMountedPath(InOutEntry.AbsoluteFilename, &CanonicalLocalPathNoExtension, &MountedPackageName,
			nullptr, nullptr, &Extension, nullptr, &MountedPathFailureReason))
		{
			if (!CanonicalLocalPathNoExtension.IsEmpty() && !Extension.IsEmpty())
			{
				InOutEntry.AbsoluteFilename = CanonicalLocalPathNoExtension + Extension;
			}
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
}

void SetMetadataFailure(FGitChangedAssetEntry& InOutEntry, const FString& InReason)
{
	if (InOutEntry.MetadataFailureReason.IsEmpty())
	{
		InOutEntry.MetadataFailureReason = InReason;
	}
}

bool TrySetOwnerLevelFromOuterPath(FGitChangedAssetEntry& InOutEntry, const FAssetData& InAssetData)
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
	InOutEntry.OwnerLevel = MoveTemp(OuterPath);
	InOutEntry.bOwnerLevelResolved = true;
	return true;
#else
	return false;
#endif
}

void ApplyAssetData(FGitChangedAssetEntry& InOutEntry, const FAssetData& InAssetData, const EGitChangedAssetMetadataSource InSource,
	const bool bAllowAssetDefinitionLookup)
{
	EnsureFallbackDisplay(InOutEntry);
	if (!InAssetData.PackageName.IsNone())
	{
		InOutEntry.PackageName = InAssetData.PackageName.ToString();
		InOutEntry.PackageKind = ClassifyPackageKind(InOutEntry.PackageName, InOutEntry.RepositoryRelativePath);
	}
	InOutEntry.MetadataSource = InSource;
	InOutEntry.bMetadataResolved = true;
	InOutEntry.MetadataFailureReason.Reset();

	const bool bIsActorDescriptor = FWorldPartitionActorDescUtils::IsValidActorDescriptorFromAssetData(InAssetData);
	if (bIsActorDescriptor)
	{
		InOutEntry.PackageKind = InOutEntry.PackageKind == EGitChangedAssetPackageKind::Unknown
			? EGitChangedAssetPackageKind::ExternalActor
			: InOutEntry.PackageKind;
		if (TUniquePtr<FWorldPartitionActorDesc> ActorDesc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(InAssetData))
		{
			const FName ActorLabelOrName = ActorDesc->GetActorLabelOrName();
			if (!ActorLabelOrName.IsNone())
			{
				InOutEntry.DisplayName = ActorLabelOrName.ToString();
			}
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
		}
	}
	else
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

bool ParseLfsPointer(const TArray<uint8>& InData, FString& OutOid, int64& OutSize)
{
	OutOid.Reset();
	OutSize = 0;
	FString Text;
	FFileHelper::BufferToString(Text, InData.GetData(), InData.Num());
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, true);
	bool bHasVersion = false;
	bool bHasOid = false;
	bool bHasSize = false;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line == TEXT("version https://git-lfs.github.com/spec/v1"))
		{
			bHasVersion = true;
		}
		else if (Line.StartsWith(TEXT("oid sha256:"), ESearchCase::CaseSensitive))
		{
			OutOid = Line.Mid(11);
			bHasOid = OutOid.Len() == 64 && Algo::AllOf(OutOid, [](const TCHAR Character) { return FChar::IsHexDigit(Character); });
		}
		else if (Line.StartsWith(TEXT("size "), ESearchCase::CaseSensitive))
		{
			bHasSize = LexTryParseString(OutSize, *Line.Mid(5)) && OutSize >= 0;
		}
	}
	return bHasVersion && bHasOid && bHasSize;
}

void AddLfsSearchRoot(TArray<FString>& InOutRoots, const FString& InRoot)
{
	if (!InRoot.IsEmpty())
	{
		InOutRoots.AddUnique(FPaths::ConvertRelativePathToFull(InRoot));
	}
}

FString FindLocalLfsObject(const FString& InRepositoryRoot, const FString& InOid, const int64 InExpectedSize)
{
	if (InOid.Len() != 64)
	{
		return FString();
	}
	TArray<FString> SearchRoots;
	const FString DotGit = FPaths::Combine(InRepositoryRoot, TEXT(".git"));
	AddLfsSearchRoot(SearchRoots, DotGit);
	if (FPaths::FileExists(DotGit))
	{
		FString GitDirFile;
		if (FFileHelper::LoadFileToString(GitDirFile, *DotGit))
		{
			GitDirFile.TrimStartAndEndInline();
			if (GitDirFile.StartsWith(TEXT("gitdir:"), ESearchCase::IgnoreCase))
			{
				FString GitDir = GitDirFile.Mid(7);
				GitDir.TrimStartAndEndInline();
				if (FPaths::IsRelative(GitDir))
				{
					GitDir = FPaths::Combine(FPaths::GetPath(DotGit), GitDir);
				}
				for (int32 ParentDepth = 0; ParentDepth < 5 && !GitDir.IsEmpty(); ++ParentDepth)
				{
					AddLfsSearchRoot(SearchRoots, GitDir);
					GitDir = FPaths::GetPath(GitDir);
				}
			}
		}
	}

	for (const FString& SearchRoot : SearchRoots)
	{
		const FString ObjectFilename = FPaths::Combine(SearchRoot, TEXT("lfs"), TEXT("objects"), InOid.Left(2), InOid.Mid(2, 2), InOid);
		if (FPaths::FileExists(ObjectFilename) && IFileManager::Get().FileSize(*ObjectFilename) == InExpectedSize)
		{
			return ObjectFilename;
		}
	}
	return FString();
}

bool LoadMetadataFromBlob(IAssetRegistry& InAssetRegistry, const FGitCatFileBatchResult& InBlob, const FString& InRepositoryRoot,
	TArray<FAssetData>& OutAssetData, FString& OutFailureReason)
{
	FString LfsOid;
	int64 LfsSize = 0;
	if (ParseLfsPointer(InBlob.Data, LfsOid, LfsSize))
	{
		const FString LocalObject = FindLocalLfsObject(InRepositoryRoot, LfsOid, LfsSize);
		if (LocalObject.IsEmpty())
		{
			OutFailureReason = TEXT("本地 Git LFS object 不可用; 列表不会自动下载 metadata。");
			return false;
		}
		IAssetRegistry::FLoadPackageRegistryData PackageData;
		InAssetRegistry.LoadPackageRegistryData(LocalObject, PackageData);
		OutAssetData = MoveTemp(PackageData.Data);
	}
	else
	{
		if (InBlob.Data.IsEmpty())
		{
			OutFailureReason = TEXT("HEAD Git blob 不包含可读取的 package metadata。");
			return false;
		}
		FLargeMemoryReader Reader(InBlob.Data.GetData(), InBlob.Data.Num(), ELargeMemoryReaderFlags::None, FName(TEXT("ChangedAssetsHead.uasset")));
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
		if (FPaths::FileExists(Entry.AbsoluteFilename))
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
		// Deleted packages receive fixed-HEAD metadata asynchronously. Do not eagerly enumerate
		// every World here when that inexpensive fast path can still resolve their owner.
		if (FPaths::FileExists(Entry.AbsoluteFilename) && !Entry.bOwnerLevelResolved)
		{
			TryDeriveStandardExternalOwnerLevel(Entry);
		}
	}
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

	TArray<FGitCatFileBatchRequest> Requests;
	TArray<int32> EntryIndices;
	for (int32 EntryIndex = 0; EntryIndex < InSnapshot.Entries.Num(); ++EntryIndex)
	{
		const FGitChangedAssetEntry& Entry = InSnapshot.Entries[EntryIndex];
		if (!FPaths::FileExists(Entry.AbsoluteFilename) && Entry.State != EGitChangedAssetState::Added && Entry.State != EGitChangedAssetState::Untracked &&
			!Entry.RepositoryRelativePath.IsEmpty())
		{
			FGitCatFileBatchRequest& Request = Requests.AddDefaulted_GetRef();
			Request.ObjectSpec = FString::Printf(TEXT("%s:%s"), *InSnapshot.PinnedHead, *Entry.RepositoryRelativePath);
			EntryIndices.Add(EntryIndex);
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
	if (Results.Num() != EntryIndices.Num())
	{
		OutError = TEXT("git cat-file 返回的 metadata result 数量不匹配。" );
		return false;
	}

	IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
	for (int32 ResultIndex = 0; ResultIndex < Results.Num(); ++ResultIndex)
	{
		if (InCancellationContext.IsValid() && InCancellationContext->IsCancellationRequested())
		{
			OutError = TEXT("Changed Assets HEAD metadata read was cancelled.");
			return false;
		}
		FGitChangedAssetHeadMetadataResult& HeadMetadata = OutResults.AddDefaulted_GetRef();
		HeadMetadata.EntryIndex = EntryIndices[ResultIndex];
		const FGitCatFileBatchResult& Result = Results[ResultIndex];
		if (!Result.bFound || Result.bSkippedBySizeLimit)
		{
			HeadMetadata.FailureReason = Result.Error.IsEmpty() ? TEXT("HEAD metadata 不可用。") : Result.Error;
			continue;
		}

		if (!LoadMetadataFromBlob(AssetRegistry, Result, InSnapshot.RepositoryRoot, HeadMetadata.AssetData, HeadMetadata.FailureReason))
		{
			continue;
		}
	}
	return true;
}

void FGitChangedAssetsMetadataResolver::ApplyHeadOnlyMetadata(FGitChangedAssetSnapshot& InOutSnapshot,
	const TArray<FGitChangedAssetHeadMetadataResult>& InResults)
{
	using namespace GitChangedAssetsMetadataPrivate;
	check(IsInGameThread());

	for (const FGitChangedAssetHeadMetadataResult& HeadMetadata : InResults)
	{
		if (!InOutSnapshot.Entries.IsValidIndex(HeadMetadata.EntryIndex))
		{
			continue;
		}
		FGitChangedAssetEntry& Entry = InOutSnapshot.Entries[HeadMetadata.EntryIndex];
		if (!HeadMetadata.FailureReason.IsEmpty())
		{
			SetMetadataFailure(Entry, HeadMetadata.FailureReason);
			continue;
		}
		if (HeadMetadata.AssetData.IsEmpty())
		{
			SetMetadataFailure(Entry, TEXT("HEAD package header 不含 Asset Registry metadata。"));
			continue;
		}
		ApplyAssetData(Entry, HeadMetadata.AssetData[0], EGitChangedAssetMetadataSource::HeadPackageRegistry, true);
	}
	for (FGitChangedAssetEntry& Entry : InOutSnapshot.Entries)
	{
		FinalizeRevertEligibility(Entry);
	}
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

void GitChangedAssetsMetadataTesting::ApplyAssetData(const FAssetData& InAssetData, FGitChangedAssetEntry& InOutEntry)
{
	check(IsInGameThread());
	GitChangedAssetsMetadataPrivate::ApplyAssetData(InOutEntry, InAssetData, EGitChangedAssetMetadataSource::CurrentAssetRegistry, true);
}
#endif
