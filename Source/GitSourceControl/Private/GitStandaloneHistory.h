// Copyright (c) 2026

#pragma once

#include "GitSourceControlHistoryMode.h"
#include "GitSourceControlRevision.h"

namespace GitSourceControlUtils
{
	bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict,
		EGitLocalSourceControlHistoryMode InMode, FString& OutCapturedHead, bool& bOutHeadChanged, TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory);
}
