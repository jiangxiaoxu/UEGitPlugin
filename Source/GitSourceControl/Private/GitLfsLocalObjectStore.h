// Copyright (c) 2026
//
// Git LFS pointer parsing and snapshot-scoped local object lookup.

#pragma once

#include "CoreMinimal.h"

enum class EGitLfsPointerParseResult : uint8
{
	NotPointer,
	ValidPointer,
	InvalidPointer,
};

enum class EGitLfsLocalObjectLookupResult : uint8
{
	Found,
	Missing,
	Error,
};

struct FGitLfsPointer final
{
	FString Oid;
	int64 Size = 0;
};

/**
 * Parse only canonical Git LFS v1 pointers. A Git blob that begins like a pointer
 * but is missing, duplicates, or corrupts a required field is never treated as a
 * normal package payload.
 */
EGitLfsPointerParseResult ParseGitLfsPointer(const TArray<uint8>& InData, FGitLfsPointer& OutPointer);
EGitLfsPointerParseResult ParseGitLfsPointerFile(const FString& InFilename, FGitLfsPointer& OutPointer);

/**
 * Resolves local LFS storage once for a repository operation and memoizes every
 * (OID, size) lookup, including misses. It never invokes Git LFS or the network.
 */
class FGitLfsLocalObjectStore final
{
public:
	FGitLfsLocalObjectStore(const FString& InGitBinary, const FString& InRepositoryRoot);

	EGitLfsLocalObjectLookupResult FindObject(const FGitLfsPointer& InPointer, FString& OutObjectFilename, FString& OutError);
	void InvalidateCachedObject(const FGitLfsPointer& InPointer);

private:
	bool ResolveStorage(FString& OutError);
	FString MakeObjectCacheKey(const FGitLfsPointer& InPointer) const;

	FString GitBinary;
	FString RepositoryRoot;
	FString LfsStorage;
	FString StorageResolutionError;
	bool bStorageResolved = false;
	bool bStorageResolutionFailed = false;
	/** Empty value is a known local miss. */
	TMap<FString, FString> ObjectLookupCache;
};
