// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlRevision.h"

#include "Algo/AllOf.h"
#include "GitLfsLocalObjectStore.h"
#include "GitStandaloneLog.h"
#include "GitSourceControlUtils.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlRevisionPrivate
{
	constexpr TCHAR TemporaryExportDirectoryName[] = TEXT("UEGitPlugin");
	constexpr TCHAR TemporaryExportFilenamePrefix[] = TEXT("UEGit-Diff-");
	FCriticalSection TemporaryExportsLock;
	TSet<FString> TemporaryExports;

	FString GetTemporaryExportDirectory()
	{
		return FPaths::Combine(FPaths::DiffDir(), TemporaryExportDirectoryName);
	}

	FString NormalizePathForComparison(const FString& InFilename)
	{
		FString Normalized = FPaths::ConvertRelativePathToFull(InFilename);
		FPaths::NormalizeFilename(Normalized);
		while (Normalized.EndsWith(TEXT("/")))
		{
			Normalized.LeftChopInline(1, EAllowShrinking::No);
		}
		return Normalized;
	}

	bool IsManagedTemporaryExport(const FString& InFilename)
	{
		if (InFilename.IsEmpty())
		{
			return false;
		}

		const FString NormalizedFilename = NormalizePathForComparison(InFilename);
		const FString NormalizedDirectory = NormalizePathForComparison(GetTemporaryExportDirectory());
		const FString DirectoryPrefix = NormalizedDirectory + TEXT("/");
		return NormalizedFilename.StartsWith(DirectoryPrefix, ESearchCase::IgnoreCase)
			&& FPaths::GetPath(NormalizedFilename).Equals(NormalizedDirectory, ESearchCase::IgnoreCase)
			&& FPaths::GetCleanFilename(NormalizedFilename).StartsWith(TemporaryExportFilenamePrefix, ESearchCase::CaseSensitive);
	}

	bool IsPackageFile(const FString& Filename)
	{
		return Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase);
	}

	bool LoadCurrentPackageForDiff(const FString& LocalFilename)
	{
		if (!IsInGameThread() || !IsPackageFile(LocalFilename))
		{
			return true;
		}

		FString PackageName;
		if (!FPackageName::TryConvertFilenameToLongPackageName(LocalFilename, PackageName))
		{
			return false;
		}
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		return Package != nullptr && Package->FindAssetInPackage() != nullptr;
	}

	bool HasValidPackageHeader(const FString& Filename)
	{
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *Filename) || Data.Num() < sizeof(uint32))
		{
			return false;
		}

		const uint32 Tag = *reinterpret_cast<const uint32*>(Data.GetData());
		return Tag == PACKAGE_FILE_TAG || Tag == PACKAGE_FILE_TAG_SWAPPED;
	}

	bool MaterializeLocalLfsObject(FGitLfsLocalObjectStore& InObjectStore, const FString& GitBinary, const FString& RepositoryRoot, const FString& PointerFilename,
		const FString& Destination, bool& bOutNeedsFetch, FString& OutError)
	{
		bOutNeedsFetch = false;
		OutError.Reset();
		FGitLfsPointer Pointer;
		if (ParseGitLfsPointerFile(PointerFilename, Pointer) != EGitLfsPointerParseResult::ValidPointer)
		{
			OutError = TEXT("The revision contains an invalid Git LFS pointer.");
			return false;
		}

		FString ObjectPath;
		const EGitLfsLocalObjectLookupResult LookupResult = InObjectStore.FindObject(Pointer, ObjectPath, OutError);
		if (LookupResult == EGitLfsLocalObjectLookupResult::Missing)
		{
			bOutNeedsFetch = true;
			OutError = FString::Printf(TEXT("Git LFS object %s is not available locally."), *Pointer.Oid);
			return false;
		}
		if (LookupResult != EGitLfsLocalObjectLookupResult::Found ||
			!GitSourceControlUtils::VerifyLocalLfsObject(GitBinary, RepositoryRoot, ObjectPath, Pointer.Oid, Pointer.Size, OutError))
		{
			return false;
		}

		if (IFileManager::Get().Copy(*Destination, *ObjectPath, true, true) != COPY_OK)
		{
			OutError = TEXT("Could not materialize the verified Git LFS object.");
			return false;
		}
		return true;
	}

	FString MakeRevisionTempFilename(const FString& CommitId, const FString& Filename)
	{
		FSHA1 Sha;
		Sha.Update(reinterpret_cast<const uint8*>(*Filename), Filename.Len() * sizeof(TCHAR));
		Sha.Final();
		uint8 Hash[FSHA1::DigestSize];
		Sha.GetHash(Hash);
		const FString Extension = FPaths::GetExtension(Filename, true);
		const FString Prefix = FString::Printf(TEXT("%s%s-%s-"), TemporaryExportFilenamePrefix, *CommitId.Left(12), *BytesToHex(Hash, UE_ARRAY_COUNT(Hash)));
		return FPaths::CreateTempFilename(*GetTemporaryExportDirectory(), *Prefix, Extension.IsEmpty() ? TEXT(".tmp") : *Extension);
	}

	void RegisterTemporaryExport(const FString& Filename)
	{
		if (!IsManagedTemporaryExport(Filename))
		{
			return;
		}
		FScopeLock Lock(&TemporaryExportsLock);
		TemporaryExports.Add(Filename);
	}

}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStandaloneRevisionUAssetScopeTest, "Cthulhu.GitSourceControl.Standalone.RevisionUAssetScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitStandaloneRevisionUAssetScopeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Revision adapter accepts .uasset"), GitSourceControlRevisionPrivate::IsPackageFile(TEXT("Content/Example.uasset")));
	TestFalse(TEXT("Revision adapter rejects future .umap scope"), GitSourceControlRevisionPrivate::IsPackageFile(TEXT("Content/Example.umap")));
	TestFalse(TEXT("Revision adapter rejects non-package files"), GitSourceControlRevisionPrivate::IsPackageFile(TEXT("Content/Example.txt")));
	const FString ManagedExport = FPaths::Combine(GitSourceControlRevisionPrivate::GetTemporaryExportDirectory(), TEXT("UEGit-Diff-test.uasset"));
	const FString OtherDirectoryFile = FPaths::Combine(FPaths::DiffDir(), TEXT("UEGit-Diff-test.uasset"));
	const FString OtherPrefixFile = FPaths::Combine(GitSourceControlRevisionPrivate::GetTemporaryExportDirectory(), TEXT("other-test.uasset"));
	TestTrue(TEXT("Session export cleanup scope is a dedicated directory and prefix"), GitSourceControlRevisionPrivate::IsManagedTemporaryExport(ManagedExport));
	TestFalse(TEXT("Session export cleanup does not include the parent Diff directory"), GitSourceControlRevisionPrivate::IsManagedTemporaryExport(OtherDirectoryFile));
	TestFalse(TEXT("Session export cleanup does not include unrelated files in its directory"), GitSourceControlRevisionPrivate::IsManagedTemporaryExport(OtherPrefixFile));
	IFileManager::Get().MakeDirectory(*GitSourceControlRevisionPrivate::GetTemporaryExportDirectory(), true);
	TestTrue(TEXT("Managed session export can be retained after a successful Diff"), FFileHelper::SaveStringToFile(TEXT("test"), *ManagedExport));
	GitSourceControlRevisionPrivate::RegisterTemporaryExport(ManagedExport);
	TestTrue(TEXT("Successful Diff export remains available until explicit cleanup"), IFileManager::Get().FileExists(*ManagedExport));
	GitSourceControlRevision::ReleaseTemporaryExport(ManagedExport);
	TestFalse(TEXT("Failed or abandoned Diff export is released immediately"), IFileManager::Get().FileExists(*ManagedExport));
	TestTrue(TEXT("Release ignores files outside the plugin-owned session directory"), FFileHelper::SaveStringToFile(TEXT("test"), *OtherDirectoryFile));
	GitSourceControlRevision::ReleaseTemporaryExport(OtherDirectoryFile);
	TestTrue(TEXT("Release leaves unrelated files untouched"), IFileManager::Get().FileExists(*OtherDirectoryFile));
	IFileManager::Get().Delete(*OtherDirectoryFile, false, true, true);
	FGitSourceControlRevision Revision;
	Revision.LocalFilename = TEXT("Content/Example.uasset");
	Revision.Filename = TEXT("Content/Example.umap");
	FString ExportFilename;
	TestFalse(TEXT("Revision export rejects a .umap historical path"), Revision.Get(ExportFilename));
	return true;
}
#endif

#if ENGINE_MAJOR_VERSION >= 5
bool FGitSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (InConcurrency != EConcurrency::Synchronous)
	{
		UE_LOG(LogGitStandalone, Verbose, TEXT("Revision export is synchronous because Unreal requires the completed file immediately."));
	}
#else
bool FGitSourceControlRevision::Get(FString& InOutFilename) const
{
#endif
	if (!GitSourceControlRevisionPrivate::IsPackageFile(Filename) || !GitSourceControlRevisionPrivate::IsPackageFile(LocalFilename))
	{
		UE_LOG(LogGitStandalone, Warning, TEXT("Revision export supports only .uasset files."));
		return false;
	}
	// Engine SourceControl history 对 workspace side 只执行 FindObject.
	// 在导出历史 package 前加载当前 package, 使原生 History/Diff 不依赖调用入口是否已打开资产.
	if (!GitSourceControlRevisionPrivate::LoadCurrentPackageForDiff(LocalFilename))
	{
		UE_LOG(LogGitStandalone, Warning, TEXT("Could not load current package '%s' before exporting a revision for Diff."), *LocalFilename);
		return false;
	}
	if (InOutFilename.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*GitSourceControlRevisionPrivate::GetTemporaryExportDirectory(), true);
		InOutFilename = FPaths::ConvertRelativePathToFull(GitSourceControlRevisionPrivate::MakeRevisionTempFilename(CommitId, Filename));
	}

	if (!ExportToFile(InOutFilename))
	{
		return false;
	}
	GitSourceControlRevisionPrivate::RegisterTemporaryExport(InOutFilename);
	return true;
}

bool FGitSourceControlRevision::ExportToFile(const FString& InFilename) const
{
	if (!GitSourceControlRevisionPrivate::IsPackageFile(Filename) || !GitSourceControlRevisionPrivate::IsPackageFile(LocalFilename) ||
		GitBinary.IsEmpty() || RepositoryRoot.IsEmpty() || CommitId.IsEmpty() || Filename.IsEmpty())
	{
		return false;
	}

	const FString TemporaryFilename = InFilename + TEXT(".git-export-tmp");
	IFileManager::Get().Delete(*TemporaryFilename, false, true, true);

	const FString RevisionSpec = FString::Printf(TEXT("%s:%s"), *CommitId, *Filename);
	FString ExportError;
	bool bSuccess = GitSourceControlUtils::DumpRevisionBlobToFile(GitBinary, RepositoryRoot, RevisionSpec, TemporaryFilename, ExportError);
	FGitLfsPointer LfsPointer;
	const EGitLfsPointerParseResult LfsPointerResult = bSuccess
		? ParseGitLfsPointerFile(TemporaryFilename, LfsPointer)
		: EGitLfsPointerParseResult::NotPointer;
	if (bSuccess && LfsPointerResult == EGitLfsPointerParseResult::InvalidPointer)
	{
		bSuccess = false;
		ExportError = TEXT("The revision contains an invalid Git LFS pointer.");
	}
	else if (bSuccess && LfsPointerResult == EGitLfsPointerParseResult::ValidPointer)
	{
		const FString MaterializedFilename = TemporaryFilename + TEXT(".lfs");
		FGitLfsLocalObjectStore LfsObjectStore(GitBinary, RepositoryRoot);
		bool bNeedsFetch = false;
		bSuccess = GitSourceControlRevisionPrivate::MaterializeLocalLfsObject(LfsObjectStore, GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsFetch, ExportError);
		if (!bSuccess && bNeedsFetch && GitSourceControlUtils::FetchLfsContentForRevision(GitBinary, RepositoryRoot, CommitId, Filename, ExportError))
		{
			LfsObjectStore.InvalidateCachedObject(LfsPointer);
			bSuccess = GitSourceControlRevisionPrivate::MaterializeLocalLfsObject(LfsObjectStore, GitBinary, RepositoryRoot, TemporaryFilename, MaterializedFilename, bNeedsFetch, ExportError);
		}
		if (bSuccess)
		{
			bSuccess = IFileManager::Get().Move(*TemporaryFilename, *MaterializedFilename, true, true, false, true);
		}
		else
		{
			IFileManager::Get().Delete(*MaterializedFilename, false, true, true);
			UE_LOG(LogGitStandalone, Warning, TEXT("Git LFS revision export failed for '%s': %s"), *Filename, *ExportError);
		}
	}

	if (bSuccess && GitSourceControlRevisionPrivate::IsPackageFile(Filename) && !GitSourceControlRevisionPrivate::HasValidPackageHeader(TemporaryFilename))
	{
		UE_LOG(LogGitStandalone, Warning, TEXT("Git revision export for '%s' is not a valid Unreal package."), *Filename);
		bSuccess = false;
	}

	if (!bSuccess)
	{
		if (!ExportError.IsEmpty())
		{
			UE_LOG(LogGitStandalone, Warning, TEXT("Git revision export failed for '%s': %s"), *Filename, *ExportError);
		}
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		return false;
	}

	if (!IFileManager::Get().Move(*InFilename, *TemporaryFilename, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
		UE_LOG(LogGitStandalone, Warning, TEXT("Could not move Git revision export '%s' into '%s'."), *TemporaryFilename, *InFilename);
		return false;
	}
	return true;
}

bool FGitSourceControlRevision::GetAnnotated(TArray<FAnnotationLine>& OutLines) const
{
	return false;
}

bool FGitSourceControlRevision::GetAnnotated(FString& InOutFilename) const
{
	return false;
}

const FString& FGitSourceControlRevision::GetFilename() const
{
	return LocalFilename.IsEmpty() ? Filename : LocalFilename;
}

int32 FGitSourceControlRevision::GetRevisionNumber() const
{
	return RevisionNumber;
}

const FString& FGitSourceControlRevision::GetRevision() const
{
	return ShortCommitId;
}

const FString& FGitSourceControlRevision::GetDescription() const
{
	return Description;
}

const FString& FGitSourceControlRevision::GetUserName() const
{
	return UserName;
}

const FString& FGitSourceControlRevision::GetClientSpec() const
{
	static const FString EmptyString;
	return EmptyString;
}

const FString& FGitSourceControlRevision::GetAction() const
{
	return Action;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlRevision::GetBranchSource() const
{
	return nullptr;
}

const FDateTime& FGitSourceControlRevision::GetDate() const
{
	return Date;
}

int32 FGitSourceControlRevision::GetCheckInIdentifier() const
{
	return CommitIdNumber;
}

int32 FGitSourceControlRevision::GetFileSize() const
{
	return FileSize;
}

void GitSourceControlRevision::CleanupTemporaryExports()
{
	TArray<FString> Files;
	{
		FScopeLock Lock(&GitSourceControlRevisionPrivate::TemporaryExportsLock);
		for (const FString& Filename : GitSourceControlRevisionPrivate::TemporaryExports)
		{
			Files.Add(Filename);
		}
		GitSourceControlRevisionPrivate::TemporaryExports.Reset();
	}
	for (const FString& Filename : Files)
	{
		ReleaseTemporaryExport(Filename);
	}

	TArray<FString> SessionExports;
	const FString ExportDirectory = GitSourceControlRevisionPrivate::GetTemporaryExportDirectory();
	IFileManager::Get().FindFilesRecursive(SessionExports, *ExportDirectory,
		*(FString(GitSourceControlRevisionPrivate::TemporaryExportFilenamePrefix) + TEXT("*")), true, false);
	for (const FString& Filename : SessionExports)
	{
		if (GitSourceControlRevisionPrivate::IsManagedTemporaryExport(Filename))
		{
			ReleaseTemporaryExport(Filename);
		}
	}
}

void GitSourceControlRevision::ReleaseTemporaryExport(const FString& Filename)
{
	if (Filename.IsEmpty())
	{
		return;
	}
	if (!GitSourceControlRevisionPrivate::IsManagedTemporaryExport(Filename))
	{
		return;
	}
	{
		FScopeLock Lock(&GitSourceControlRevisionPrivate::TemporaryExportsLock);
		GitSourceControlRevisionPrivate::TemporaryExports.Remove(Filename);
	}
	IFileManager::Get().Delete(*Filename, false, true, true);
	IFileManager::Get().Delete(*(Filename + TEXT(".git-export-tmp")), false, true, true);
}

#undef LOCTEXT_NAMESPACE
