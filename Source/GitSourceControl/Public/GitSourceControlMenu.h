// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

class FMenuBuilder;
struct FAssetData;
struct FToolMenuSection;
class UToolMenu;
class FGitSourceControlMenuLifetimeState;

/** Asset-focused Git actions exposed in the Editor. */
class FGitSourceControlMenu
{
public:
	void Register();
	void Unregister();

private:
	TSharedRef<class FExtender> OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);
	void AddAssetMenuEntries(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets);
	void AddHistoryMenuEntries(UToolMenu* InToolMenu);
	void AddToolbarEntries(FToolMenuSection& InSection);

	void DiscardSelectedAssets(TArray<FAssetData> SelectedAssets);
	void DeleteSelectedUntrackedAssets(TArray<FAssetData> SelectedAssets);
	void ViewSelectedAssetHistory(TArray<FAssetData> SelectedAssets);
	void RestoreRevisionToWorkspace(FString Filename, FString Revision);
	void RefreshSelectedAssets(TArray<FAssetData> SelectedAssets);
	void RefreshCachedLocalState();
	void DiscardCachedGitChanges();

	FDelegateHandle AssetMenuExtenderHandle;
	TSharedPtr<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState;
};
