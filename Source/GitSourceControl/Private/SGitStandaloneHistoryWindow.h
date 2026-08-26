// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "GitSourceControlHistoryMode.h"
#include "GitSourceControlRevision.h"

class SWindow;
namespace GitSourceControlUtils
{
	class FGitOperationCancellationContext;
}

using FGitStandaloneHistoryRevisionPtr = TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe>;
using FGitStandaloneHistoryDiffCancellationPtr = TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>;

DECLARE_DELEGATE_ThreeParams(FGitStandaloneHistoryRestoreDelegate, FString, FString, FString);
DECLARE_DELEGATE_TwoParams(FGitStandaloneHistoryRefreshDelegate, FString, EGitLocalSourceControlHistoryMode);
DECLARE_DELEGATE_RetVal_ThreeParams(FGitStandaloneHistoryDiffCancellationPtr, FGitStandaloneHistoryDiffDelegate,
	FString, FGitStandaloneHistoryRevisionPtr, FGitStandaloneHistoryRevisionPtr);

/** Creates the standalone Git History window without registering an Unreal Source Control provider. */
namespace GitSourceControlStandaloneHistory
{
	TSharedRef<SWindow> CreateWindow(const FString& Filename, EGitLocalSourceControlHistoryMode Mode,
		const TGitSourceControlHistory& History, FGitStandaloneHistoryRestoreDelegate OnRestore, FGitStandaloneHistoryRefreshDelegate OnRefresh,
		FGitStandaloneHistoryDiffDelegate OnDiff);
}
