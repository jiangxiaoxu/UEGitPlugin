// Copyright (c) 2026

#include "GitLfsLocalObjectStore.h"

#include "Algo/AllOf.h"
#include "GitSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace GitLfsLocalObjectStorePrivate
{
	constexpr int64 MaxPointerBytes = 4 * 1024;
	constexpr ANSICHAR VersionPrefix[] = "version ";
	constexpr ANSICHAR OidLinePrefix[] = "oid ";
	constexpr ANSICHAR SizeLinePrefix[] = "size ";
	constexpr TCHAR CanonicalVersion[] = TEXT("version https://git-lfs.github.com/spec/v1");
	constexpr TCHAR OidPrefix[] = TEXT("oid sha256:");
	constexpr TCHAR SizePrefix[] = TEXT("size ");

	bool StartsWith(const TArray<uint8>& InData, const ANSICHAR* InPrefix, const int32 InPrefixLength)
	{
		return InData.Num() >= InPrefixLength && FMemory::Memcmp(InData.GetData(), InPrefix, InPrefixLength) == 0;
	}

	bool LooksLikePointer(const TArray<uint8>& InData)
	{
		return StartsWith(InData, VersionPrefix, UE_ARRAY_COUNT(VersionPrefix) - 1) ||
			StartsWith(InData, OidLinePrefix, UE_ARRAY_COUNT(OidLinePrefix) - 1) ||
			StartsWith(InData, SizeLinePrefix, UE_ARRAY_COUNT(SizeLinePrefix) - 1);
	}

	bool ParseSize(const FString& InValue, int64& OutSize)
	{
		if (InValue.IsEmpty())
		{
			return false;
		}
		int64 Value = 0;
		for (const TCHAR Character : InValue)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const int64 Digit = Character - TEXT('0');
			if (Value > (MAX_int64 - Digit) / 10)
			{
				return false;
			}
			Value = Value * 10 + Digit;
		}
		OutSize = Value;
		return true;
	}

	bool IsValidOid(const FString& InOid)
	{
		return InOid.Len() == 64 && Algo::AllOf(InOid, [](const TCHAR Character)
		{
			return FChar::IsHexDigit(Character);
		});
	}

	bool IsPointerByte(const uint8 InByte)
	{
		return InByte == '\n' || InByte == '\r' || (InByte >= 0x20 && InByte <= 0x7e);
	}

	bool ReadPrefixLooksLikePointer(const FString& InFilename)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*InFilename));
		if (!Reader)
		{
			return true;
		}
		uint8 Prefix[UE_ARRAY_COUNT(VersionPrefix) - 1] = {};
		Reader->Serialize(Prefix, UE_ARRAY_COUNT(Prefix));
		if (Reader->IsError())
		{
			return true;
		}
		TArray<uint8> PrefixData;
		PrefixData.Append(Prefix, UE_ARRAY_COUNT(Prefix));
		return LooksLikePointer(PrefixData);
	}
}

EGitLfsPointerParseResult ParseGitLfsPointer(const TArray<uint8>& InData, FGitLfsPointer& OutPointer)
{
	using namespace GitLfsLocalObjectStorePrivate;

	OutPointer = FGitLfsPointer();
	if (!LooksLikePointer(InData))
	{
		return EGitLfsPointerParseResult::NotPointer;
	}
	if (InData.Num() > MaxPointerBytes || InData.IsEmpty() || !Algo::AllOf(InData, IsPointerByte))
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}

	FString PointerText;
	FFileHelper::BufferToString(PointerText, InData.GetData(), InData.Num());
	PointerText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	if (PointerText.Contains(TEXT("\r")))
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}
	if (PointerText.EndsWith(TEXT("\n")))
	{
		PointerText.LeftChopInline(1, EAllowShrinking::No);
	}
	if (PointerText.IsEmpty() || PointerText.EndsWith(TEXT("\n")))
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}

	TArray<FString> Lines;
	PointerText.ParseIntoArray(Lines, TEXT("\n"), false);
	if (Lines.Num() != 3 || Lines[0] != CanonicalVersion || !Lines[1].StartsWith(OidPrefix, ESearchCase::CaseSensitive) ||
		!Lines[2].StartsWith(SizePrefix, ESearchCase::CaseSensitive))
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}
	OutPointer.Oid = Lines[1].Mid(UE_ARRAY_COUNT(OidPrefix) - 1);
	if (!IsValidOid(OutPointer.Oid) || !ParseSize(Lines[2].Mid(UE_ARRAY_COUNT(SizePrefix) - 1), OutPointer.Size))
	{
		OutPointer = FGitLfsPointer();
		return EGitLfsPointerParseResult::InvalidPointer;
	}
	return EGitLfsPointerParseResult::ValidPointer;
}

EGitLfsPointerParseResult ParseGitLfsPointerFile(const FString& InFilename, FGitLfsPointer& OutPointer)
{
	OutPointer = FGitLfsPointer();
	const int64 FileSize = IFileManager::Get().FileSize(*InFilename);
	if (FileSize < 0)
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}
	if (FileSize > GitLfsLocalObjectStorePrivate::MaxPointerBytes)
	{
		return GitLfsLocalObjectStorePrivate::ReadPrefixLooksLikePointer(InFilename)
			? EGitLfsPointerParseResult::InvalidPointer
			: EGitLfsPointerParseResult::NotPointer;
	}
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *InFilename))
	{
		return EGitLfsPointerParseResult::InvalidPointer;
	}
	return ParseGitLfsPointer(Data, OutPointer);
}

FGitLfsLocalObjectStore::FGitLfsLocalObjectStore(const FString& InGitBinary, const FString& InRepositoryRoot)
	: GitBinary(InGitBinary)
	, RepositoryRoot(FPaths::ConvertRelativePathToFull(InRepositoryRoot))
{
	FPaths::NormalizeDirectoryName(RepositoryRoot);
}

EGitLfsLocalObjectLookupResult FGitLfsLocalObjectStore::FindObject(const FGitLfsPointer& InPointer, FString& OutObjectFilename, FString& OutError)
{
	OutObjectFilename.Reset();
	OutError.Reset();
	if (InPointer.Size < 0 || !GitLfsLocalObjectStorePrivate::IsValidOid(InPointer.Oid))
	{
		OutError = TEXT("Invalid Git LFS pointer metadata.");
		return EGitLfsLocalObjectLookupResult::Error;
	}
	if (!ResolveStorage(OutError))
	{
		return EGitLfsLocalObjectLookupResult::Error;
	}

	const FString CacheKey = MakeObjectCacheKey(InPointer);
	if (const FString* CachedObject = ObjectLookupCache.Find(CacheKey))
	{
		if (CachedObject->IsEmpty())
		{
			return EGitLfsLocalObjectLookupResult::Missing;
		}
		OutObjectFilename = *CachedObject;
		return EGitLfsLocalObjectLookupResult::Found;
	}

	const FString ObjectFilename = FPaths::Combine(LfsStorage, TEXT("objects"), InPointer.Oid.Left(2), InPointer.Oid.Mid(2, 2), InPointer.Oid);
	if (!IFileManager::Get().FileExists(*ObjectFilename))
	{
		ObjectLookupCache.Add(CacheKey, FString());
		return EGitLfsLocalObjectLookupResult::Missing;
	}
	if (IFileManager::Get().FileSize(*ObjectFilename) != InPointer.Size)
	{
		OutError = FString::Printf(TEXT("The local Git LFS object has an unexpected size for %s."), *InPointer.Oid);
		return EGitLfsLocalObjectLookupResult::Error;
	}

	ObjectLookupCache.Add(CacheKey, ObjectFilename);
	OutObjectFilename = ObjectFilename;
	return EGitLfsLocalObjectLookupResult::Found;
}

void FGitLfsLocalObjectStore::InvalidateCachedObject(const FGitLfsPointer& InPointer)
{
	ObjectLookupCache.Remove(MakeObjectCacheKey(InPointer));
}

bool FGitLfsLocalObjectStore::ResolveStorage(FString& OutError)
{
	OutError.Reset();
	if (bStorageResolved)
	{
		return true;
	}
	if (bStorageResolutionFailed)
	{
		OutError = StorageResolutionError;
		return false;
	}

	FString StorageOutput;
	FString StorageError;
	if (GitSourceControlUtils::RunCommandInternalRaw(TEXT("config"), GitBinary, RepositoryRoot,
		{ TEXT("--path"), TEXT("--get"), TEXT("lfs.storage") }, {}, StorageOutput, StorageError, 0, false))
	{
		LfsStorage = MoveTemp(StorageOutput);
		LfsStorage.TrimStartAndEndInline();
	}
	if (LfsStorage.IsEmpty())
	{
		FString CommonGitDirectory;
		if (!GitSourceControlUtils::RunCommandInternalRaw(TEXT("rev-parse"), GitBinary, RepositoryRoot,
			{ TEXT("--git-common-dir") }, {}, CommonGitDirectory, StorageError, 0, false))
		{
			StorageResolutionError = StorageError.IsEmpty()
				? TEXT("Could not resolve the local Git common directory for Git LFS.")
				: StorageError;
			bStorageResolutionFailed = true;
			OutError = StorageResolutionError;
			return false;
		}
		CommonGitDirectory.TrimStartAndEndInline();
		if (CommonGitDirectory.IsEmpty())
		{
			StorageResolutionError = TEXT("Git did not return a common directory for Git LFS.");
			bStorageResolutionFailed = true;
			OutError = StorageResolutionError;
			return false;
		}
		if (FPaths::IsRelative(CommonGitDirectory))
		{
			CommonGitDirectory = FPaths::ConvertRelativePathToFull(RepositoryRoot, CommonGitDirectory);
		}
		LfsStorage = FPaths::Combine(CommonGitDirectory, TEXT("lfs"));
	}
	if (FPaths::IsRelative(LfsStorage))
	{
		LfsStorage = FPaths::ConvertRelativePathToFull(RepositoryRoot, LfsStorage);
	}
	FPaths::NormalizeDirectoryName(LfsStorage);
	bStorageResolved = true;
	return true;
}

FString FGitLfsLocalObjectStore::MakeObjectCacheKey(const FGitLfsPointer& InPointer) const
{
	return InPointer.Oid.ToLower() + TEXT(":") + LexToString(InPointer.Size);
}

bool FGitLfsBatchVerificationContext::IsVerified(const FGitLfsPointer& InPointer) const
{
	return VerifiedObjectKeys.Contains(MakeObjectKey(InPointer));
}

void FGitLfsBatchVerificationContext::MarkVerified(const FGitLfsPointer& InPointer)
{
	VerifiedObjectKeys.Add(MakeObjectKey(InPointer));
}

FString FGitLfsBatchVerificationContext::MakeObjectKey(const FGitLfsPointer& InPointer)
{
	return InPointer.Oid.ToLower() + TEXT(":") + LexToString(InPointer.Size);
}
