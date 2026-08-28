// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableViewBase.h"

class FGitChangedAssetsController;
struct FGitChangedAssetEntry;
template <typename ItemType> class SListView;
template <typename ItemType> class SComboBox;
class SSearchBox;
class STableViewBase;
class SWidget;
class ITableRow;
struct FPointerEvent;

/** Virtualized providerless view over one explicit Git Changed Assets snapshot. */
class SGitChangedAssetsPanel final : public SCompoundWidget
{
public:
	using FControllerPtr = TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe>;

	SLATE_BEGIN_ARGS(SGitChangedAssetsPanel) {}
		SLATE_ARGUMENT(FControllerPtr, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SGitChangedAssetsPanel() override;

private:
	using FEntryPtr = TSharedPtr<FGitChangedAssetEntry>;

	void HandleControllerRowsChanged();
	void HandleControllerActivityChanged();
	void RequestInitialRefreshIfAvailable();
	void RebuildItems();
	bool RebuildOwnerOptions();
	void ApplyFilters(bool bClearSelection);
	bool MatchesFilters(const FGitChangedAssetEntry& Entry) const;
	void ClearListSelection();
	void PruneListSelectionToFilteredItems();
	void ReplaceListSelection(const TArray<FEntryPtr>& NewSelection, const FEntryPtr& UserDirectedItem);
	TArray<FEntryPtr> GetSelectedEntries() const;
	FEntryPtr GetListRangeSelectionAnchor() const;
	void RebaseListNavigationState(const FEntryPtr& NewAnchor);
	void HandleListSelectionChanged(FEntryPtr Item, ESelectInfo::Type SelectInfo);

	TSharedRef<ITableRow> GenerateRow(FEntryPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void HandleSearchChanged(const FText& InText);
	void HandleStatusChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void HandleKindChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void HandleOwnerChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	void HandleRevertableChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> GenerateFilterOption(TSharedPtr<FString> Option) const;

	FReply HandleRefreshClicked();
	FReply HandleCopyPathsClicked();
	FReply HandleRevertClicked();
	bool IsStartupGitCapabilityAvailable() const;
	bool HasSelection() const;
	bool CanRevertSelection() const;

public:
	/** Row widgets route clicks here so a normal click toggles while preserving existing selections. */
	FReply HandleEntryMouseButtonDown(FEntryPtr Entry, const FPointerEvent& MouseEvent);
	/** The list background clears the custom mouse range anchor even when Slate has no selected item to clear. */
	void HandleListBlankMouseButtonDown();
	ECheckBoxState GetEntryCheckState(FEntryPtr Entry) const;

private:
	FText GetStatusText() const;
	FText GetErrorText() const;
	EVisibility GetErrorVisibility() const;
	FText GetCopyPathsButtonText() const;
	FText GetRevertButtonText() const;

	FControllerPtr Controller;
	FDelegateHandle ControllerRowsChangedHandle;
	FDelegateHandle ControllerActivityChangedHandle;
	TArray<FEntryPtr> AllItems;
	TArray<FEntryPtr> FilteredItems;
	/** Stable only within the displayed snapshot generation; protects selection across metadata enrichment. */
	TMap<FString, FEntryPtr> ItemsByAbsoluteFilename;
	/** Cached native ListView range anchor used only for custom file-explorer mouse semantics. */
	FEntryPtr SelectionAnchor;
	bool bApplyingListSelection = false;
	bool bUpdatingOwnerOptions = false;
	bool bInitialRefreshRequested = false;
	TArray<TSharedPtr<FString>> StatusOptions;
	TArray<TSharedPtr<FString>> KindOptions;
	TArray<TSharedPtr<FString>> OwnerOptions;
	TArray<TSharedPtr<FString>> RevertableOptions;
	TSharedPtr<FString> SelectedStatus;
	TSharedPtr<FString> SelectedKind;
	TSharedPtr<FString> SelectedOwner;
	TSharedPtr<FString> SelectedRevertable;
	FString SearchText;
	uint64 DisplayedSnapshotGeneration = MAX_uint64;
	TSharedPtr<SListView<FEntryPtr>> ListView;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> StatusComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> KindComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> OwnerComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> RevertableComboBox;
};
