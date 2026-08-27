// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"

namespace GitSourceControlUtils
{
	class FGitOperationCancellationContext;
}

/** One immutable Git object specification requested from `git cat-file --batch-command`. */
struct FGitCatFileBatchRequest
{
	/** e.g. <full-head-sha>:Content/Asset.uasset. Must not contain a NUL. */
	FString ObjectSpec;
};

/** The local, raw Git blob returned for one request. */
struct FGitCatFileBatchResult
{
	FString ObjectSpec;
	FString ObjectId;
	bool bFound = false;
	bool bSkippedBySizeLimit = false;
	int64 BlobSize = 0;
	TArray<uint8> Data;
	FString Error;
};

/**
 * Binary-safe, bounded reader for one short-lived `git cat-file --batch-command -Z` process.
 * It deliberately does not invoke Git LFS or any network operation.
 */
class FGitCatFileBatchReader final
{
public:
	/**
	 * Query all object specifications through one process. A missing or oversized individual
	 * object is represented in its result and does not fail the entire batch.
	 */
	static bool ReadBlobs(const FString& InGitBinary, const FString& InRepositoryRoot,
		const TArray<FGitCatFileBatchRequest>& InRequests, TArray<FGitCatFileBatchResult>& OutResults,
		FString& OutError, int64 InMaxBlobBytes = 1024 * 1024, int64 InMaxTotalBlobBytes = 8 * 1024 * 1024,
		double InTimeoutSeconds = 30.0,
		TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> InCancellationContext = nullptr);
};
