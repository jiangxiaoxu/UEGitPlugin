// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "ToolMenuEntry.h"

class FGitSourceControlStatusBarIntegration final
{
public:
	FGitSourceControlStatusBarIntegration(FSimpleDelegate InOpenChangedAssets);
	~FGitSourceControlStatusBarIntegration();

	/** 注册生成前兜底并立即尝试替换。 */
	void Install();
	/** 移除兜底，并在安全时精确恢复原 entry。 */
	void Uninstall();

private:
	void HandlePreGenerateWidget(const FName InMenuName, const struct FToolMenuContext& InMenuContext);
	bool TryReplaceEntry();
	void RestoreOriginalEntry();

	FSimpleDelegate OpenChangedAssets;
	FDelegateHandle PreGenerateHandle;
	TOptional<FToolMenuEntry> OriginalEntry;
	FName OriginalSectionName = NAME_None;
	int32 OriginalBlockIndex = INDEX_NONE;
};
