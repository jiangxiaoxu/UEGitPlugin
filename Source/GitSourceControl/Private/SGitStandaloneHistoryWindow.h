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

/** 一次 standalone History 调用明确支持的上下文级操作. */
struct FGitStandaloneHistoryWindowCapabilities
{
	bool bAllowRestore = false;
};

DECLARE_DELEGATE_ThreeParams(FGitStandaloneHistoryRestoreDelegate, FString, FString, FString);
DECLARE_DELEGATE_TwoParams(FGitStandaloneHistoryRefreshDelegate, FString, EGitLocalSourceControlHistoryMode);
DECLARE_DELEGATE_RetVal_ThreeParams(FGitStandaloneHistoryDiffCancellationPtr, FGitStandaloneHistoryDiffDelegate,
	FString, FGitStandaloneHistoryRevisionPtr, FGitStandaloneHistoryRevisionPtr);

/** Creates the standalone Git History window without registering an Unreal Source Control provider. */
namespace GitSourceControlStandaloneHistory
{
	/** Restore action 的上下文可见性, 供菜单和自动化测试复用. */
	GITSOURCECONTROL_API bool CanShowRestoreAction(FGitStandaloneHistoryWindowCapabilities Capabilities, bool bRestoreDelegateBound,
		const FString& Filename);

	TSharedRef<SWindow> CreateWindow(const FString& Filename, EGitLocalSourceControlHistoryMode Mode,
		const TGitSourceControlHistory& History, FGitStandaloneHistoryRestoreDelegate OnRestore, FGitStandaloneHistoryRefreshDelegate OnRefresh,
		FGitStandaloneHistoryDiffDelegate OnDiff, FGitStandaloneHistoryWindowCapabilities Capabilities);
}
