// Copyright (c) 2026

#include "SGitChangedAssetsPanel.h"

#include "GitChangedAssetsController.h"
#include "GitChangedAssetsModel.h"
#include "GitSourceControlUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

class SChangedAssetsListView final : public SListView<TSharedPtr<FGitChangedAssetEntry>>
{
public:
	using Super = SListView<TSharedPtr<FGitChangedAssetEntry>>;
	using FEntryPtr = TSharedPtr<FGitChangedAssetEntry>;

	void Construct(const FArguments& InArgs, const TWeakPtr<SGitChangedAssetsPanel>& InPanel)
	{
		Panel = InPanel;
		Super::Construct(InArgs);
	}

	FEntryPtr GetRangeSelectionAnchor() const
	{
		return RangeSelectionStart;
	}

	void RebaseNavigationState(const FEntryPtr& NewAnchor)
	{
		SelectorItem = NewAnchor;
		RangeSelectionStart = NewAnchor;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && !MouseEvent.IsControlDown() && !MouseEvent.IsShiftDown())
		{
			if (const TSharedPtr<SGitChangedAssetsPanel> PinnedPanel = Panel.Pin())
			{
				PinnedPanel->HandleListBlankMouseButtonDown();
			}
		}
		return Super::OnMouseButtonDown(MyGeometry, MouseEvent);
	}

private:
	TWeakPtr<SGitChangedAssetsPanel> Panel;
};

namespace SGitChangedAssetsPanelPrivate
{
	using FEntryPtr = TSharedPtr<FGitChangedAssetEntry>;

	const FName NameColumn(TEXT("Name"));
	const FName OwnerColumn(TEXT("OwnerLevel"));
	const FName PathColumn(TEXT("AssetPath"));
	const FName TypeColumn(TEXT("Type"));
	const FName StatusColumn(TEXT("Status"));

	FString GetPackageKindText(const EGitChangedAssetPackageKind Kind)
	{
		switch (Kind)
		{
		case EGitChangedAssetPackageKind::Regular:
			return TEXT("Regular asset");
		case EGitChangedAssetPackageKind::ExternalActor:
			return TEXT("External actor");
		case EGitChangedAssetPackageKind::ExternalObject:
			return TEXT("External object");
		default:
			return TEXT("Unknown");
		}
	}

	FSlateColor GetStateColor(const EGitChangedAssetState State)
	{
		switch (State)
		{
		case EGitChangedAssetState::Added:
		case EGitChangedAssetState::Untracked:
			return FSlateColor(FLinearColor(0.32f, 0.85f, 0.32f));
		case EGitChangedAssetState::Deleted:
		case EGitChangedAssetState::Conflicted:
			return FSlateColor(FLinearColor(0.95f, 0.28f, 0.28f));
		case EGitChangedAssetState::Renamed:
			return FSlateColor(FLinearColor(0.45f, 0.70f, 1.0f));
		default:
			return FSlateColor::UseForeground();
		}
	}

	FString BuildTooltip(const FGitChangedAssetEntry& Entry)
	{
		FString Result = Entry.RepositoryRelativePath;
		if (!Entry.RenameFromRepositoryRelativePath.IsEmpty())
		{
			Result += FString::Printf(TEXT("\nRename source: %s"), *Entry.RenameFromRepositoryRelativePath);
		}
		if (!Entry.RevertBlockReason.IsEmpty())
		{
			Result += FString::Printf(TEXT("\nRevert unavailable: %s"), *Entry.RevertBlockReason);
		}
		if (!Entry.MetadataFailureReason.IsEmpty())
		{
			Result += FString::Printf(TEXT("\nMetadata: %s"), *Entry.MetadataFailureReason);
		}
		if (!Entry.FullDataLayerNames.IsEmpty())
		{
			Result += FString::Printf(TEXT("\nData Layers: %s"), *Entry.FullDataLayerNames);
		}
		return Result;
	}

	FString GetOwnerLevelDisplayText(const FString& OwnerLevel)
	{
		if (OwnerLevel.IsEmpty())
		{
			return TEXT("—");
		}
		const FString ShortName = FPackageName::GetShortName(OwnerLevel);
		return ShortName.IsEmpty() ? OwnerLevel : ShortName;
	}

	FString GetObjectPathDisplayText(const FGitChangedAssetEntry& Entry)
	{
		if (!Entry.DisplayObjectPath.IsEmpty())
		{
			return Entry.DisplayObjectPath;
		}
		FString DisplayPath = Entry.ObjectPath.IsEmpty() ? Entry.RepositoryRelativePath : Entry.ObjectPath;
		if (Entry.PackageKind != EGitChangedAssetPackageKind::ExternalActor
			&& Entry.PackageKind != EGitChangedAssetPackageKind::ExternalObject)
		{
			return DisplayPath;
		}

		const int32 UaidIndex = DisplayPath.Find(TEXT("_UAID_"), ESearchCase::IgnoreCase);
		if (UaidIndex != INDEX_NONE)
		{
			int32 SubObjectIndex = INDEX_NONE;
			if (DisplayPath.FindLastChar(TEXT(':'), SubObjectIndex) && SubObjectIndex < UaidIndex)
			{
				DisplayPath = DisplayPath.Mid(SubObjectIndex + 1, UaidIndex - SubObjectIndex - 1);
			}
			else
			{
				DisplayPath.LeftInline(UaidIndex, EAllowShrinking::No);
			}
		}
		return DisplayPath;
	}

	TArray<FString> BuildSelectedAbsolutePaths(const TArray<FEntryPtr>& InFilteredItems, const TArray<FEntryPtr>& InSelectedItems)
	{
		TArray<FString> Paths;
		for (const FEntryPtr& Item : InFilteredItems)
		{
			if (Item.IsValid() && !Item->AbsoluteFilename.IsEmpty()
				&& InSelectedItems.ContainsByPredicate([&Item](const FEntryPtr& Selected) { return Selected == Item; }))
			{
				FString AbsolutePath = FPaths::ConvertRelativePathToFull(Item->AbsoluteFilename);
				FPaths::MakePlatformFilename(AbsolutePath);
				Paths.Add(MoveTemp(AbsolutePath));
			}
		}
		return Paths;
	}

	bool ContainsEntry(const TArray<FEntryPtr>& Entries, const FEntryPtr& Entry)
	{
		return Entries.ContainsByPredicate([&Entry](const FEntryPtr& Candidate)
		{
			return Candidate == Entry;
		});
	}

	void AddEntryIfMissing(TArray<FEntryPtr>& Entries, const FEntryPtr& Entry)
	{
		if (Entry.IsValid() && !ContainsEntry(Entries, Entry))
		{
			Entries.Add(Entry);
		}
	}

	TArray<FEntryPtr> BuildMouseSelection(const TArray<FEntryPtr>& InFilteredItems, const TArray<FEntryPtr>& InCurrentSelection,
		const FEntryPtr& InAnchor, const FEntryPtr& InClickedItem, const bool bShiftDown, const bool bControlDown)
	{
		TArray<FEntryPtr> CurrentSelection;
		for (const FEntryPtr& Item : InFilteredItems)
		{
			if (ContainsEntry(InCurrentSelection, Item))
			{
				AddEntryIfMissing(CurrentSelection, Item);
			}
		}

		if (!InClickedItem.IsValid())
		{
			return CurrentSelection;
		}

		if (!bShiftDown)
		{
			if (ContainsEntry(CurrentSelection, InClickedItem))
			{
				CurrentSelection.RemoveAll([&InClickedItem](const FEntryPtr& Item)
				{
					return Item == InClickedItem;
				});
			}
			else
			{
				AddEntryIfMissing(CurrentSelection, InClickedItem);
			}
			return CurrentSelection;
		}

		int32 AnchorIndex = InFilteredItems.IndexOfByKey(InAnchor);
		const int32 ClickedIndex = InFilteredItems.IndexOfByKey(InClickedItem);
		if (ClickedIndex == INDEX_NONE)
		{
			return CurrentSelection;
		}
		if (AnchorIndex == INDEX_NONE)
		{
			AnchorIndex = ClickedIndex;
		}

		const int32 RangeBegin = FMath::Min(AnchorIndex, ClickedIndex);
		const int32 RangeEnd = FMath::Max(AnchorIndex, ClickedIndex);
		TArray<FEntryPtr> Result = bControlDown ? CurrentSelection : TArray<FEntryPtr>();
		for (int32 Index = RangeBegin; Index <= RangeEnd; ++Index)
		{
			AddEntryIfMissing(Result, InFilteredItems[Index]);
		}
		return Result;
	}

	TArray<FEntryPtr> IntersectSelectionWithFilteredItems(const TArray<FEntryPtr>& FilteredItems, const TArray<FEntryPtr>& CurrentSelection)
	{
		TArray<FEntryPtr> Result;
		for (const FEntryPtr& Item : FilteredItems)
		{
			if (ContainsEntry(CurrentSelection, Item))
			{
				AddEntryIfMissing(Result, Item);
			}
		}
		return Result;
	}

	FString GetEntryIdentity(const FGitChangedAssetEntry& Entry)
	{
		return Entry.AbsoluteFilename.IsEmpty() ? Entry.RepositoryRelativePath : Entry.AbsoluteFilename;
	}

	void ReconcileItemsByIdentity(const TArray<FGitChangedAssetEntry>& Entries, TMap<FString, FEntryPtr>& InOutItemsByIdentity,
		TArray<FEntryPtr>& OutItems)
	{
		OutItems.Reset();
		OutItems.Reserve(Entries.Num());
		for (const FGitChangedAssetEntry& Entry : Entries)
		{
			const FString Identity = GetEntryIdentity(Entry);
			FEntryPtr& ExistingItem = InOutItemsByIdentity.FindOrAdd(Identity);
			if (ExistingItem.IsValid())
			{
				*ExistingItem = Entry;
			}
			else
			{
				ExistingItem = MakeShared<FGitChangedAssetEntry>(Entry);
			}
			OutItems.Add(ExistingItem);
		}
	}

	TSharedPtr<FString> FindOptionByValue(const TArray<TSharedPtr<FString>>& Options, const FString& Value)
	{
		if (const TSharedPtr<FString>* Existing = Options.FindByPredicate([&Value](const TSharedPtr<FString>& Candidate)
		{
			return Candidate.IsValid() && Candidate->Equals(Value, ESearchCase::CaseSensitive);
		}))
		{
			return *Existing;
		}
		return nullptr;
	}

	struct FOwnerOptionsBuildResult
	{
		TArray<TSharedPtr<FString>> Options;
		TSharedPtr<FString> SelectedOption;
		bool bOwnerFilterChanged = false;
	};

	FOwnerOptionsBuildResult BuildOwnerOptions(const TArray<TSharedPtr<FString>>& PreviousOptions, const TSharedPtr<FString>& PreviousSelectedOption,
		const TArray<FEntryPtr>& Items)
	{
		FOwnerOptionsBuildResult Result;
		const FString PreviousSelection = PreviousSelectedOption.IsValid() ? *PreviousSelectedOption : TEXT("All owner levels");
		const auto ReuseOrAddOption = [&PreviousOptions, &Result](const FString& Value)
		{
			const TSharedPtr<FString> ExistingOption = FindOptionByValue(PreviousOptions, Value);
			if (ExistingOption.IsValid())
			{
				return Result.Options.Add_GetRef(ExistingOption);
			}
			return Result.Options.Add_GetRef(MakeShared<FString>(Value));
		};

		ReuseOrAddOption(TEXT("All owner levels"));
		ReuseOrAddOption(TEXT("<No owner level>"));
		TArray<FString> OwnerLevels;
		for (const FEntryPtr& Item : Items)
		{
			if (Item.IsValid() && !Item->OwnerLevel.IsEmpty())
			{
				OwnerLevels.AddUnique(Item->OwnerLevel);
			}
		}
		OwnerLevels.Sort();
		for (const FString& OwnerLevel : OwnerLevels)
		{
			ReuseOrAddOption(OwnerLevel);
		}

		Result.SelectedOption = FindOptionByValue(Result.Options, PreviousSelection);
		if (!Result.SelectedOption.IsValid())
		{
			Result.SelectedOption = FindOptionByValue(Result.Options, TEXT("All owner levels"));
		}
		Result.bOwnerFilterChanged = !Result.SelectedOption.IsValid()
			|| !Result.SelectedOption->Equals(PreviousSelection, ESearchCase::CaseSensitive);
		return Result;
	}

	FEntryPtr ResolveSelectionAnchor(const TArray<FEntryPtr>& FilteredItems, const TArray<FEntryPtr>& SelectedItems, const FEntryPtr& NativeRangeAnchor)
	{
		if (SelectedItems.IsEmpty())
		{
			return nullptr;
		}
		if (ContainsEntry(FilteredItems, NativeRangeAnchor))
		{
			return NativeRangeAnchor;
		}
		for (const FEntryPtr& Item : FilteredItems)
		{
			if (ContainsEntry(SelectedItems, Item))
			{
				return Item;
			}
		}
		return nullptr;
	}

	class SChangedAssetRow final : public SMultiColumnTableRow<TSharedPtr<FGitChangedAssetEntry>>
	{
	public:
		SLATE_BEGIN_ARGS(SChangedAssetRow) {}
			SLATE_ARGUMENT(TSharedPtr<FGitChangedAssetEntry>, Item)
			SLATE_ARGUMENT(TWeakPtr<SGitChangedAssetsPanel>, Panel)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
		{
			Item = InArgs._Item;
			Panel = InArgs._Panel;
			SMultiColumnTableRow<TSharedPtr<FGitChangedAssetEntry>>::Construct(FSuperRowType::FArguments(), InOwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			if (!Item.IsValid())
			{
				return SNew(STextBlock).Text(LOCTEXT("InvalidChangedAsset", "Invalid Changed Asset"));
			}

			const TWeakPtr<SGitChangedAssetsPanel> WeakPanel = Panel;
			if (ColumnName == NameColumn)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f)
					[
						SNew(SCheckBox)
						.Visibility(EVisibility::HitTestInvisible)
						.IsChecked_Lambda([WeakPanel, RowItem = Item]()
						{
							if (const TSharedPtr<SGitChangedAssetsPanel> PinnedPanel = WeakPanel.Pin())
							{
								return PinnedPanel->GetEntryCheckState(RowItem);
							}
							return ECheckBoxState::Unchecked;
						})
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([RowItem = Item]()
						{
							return FText::FromString(RowItem->DisplayName.IsEmpty() ? RowItem->RepositoryRelativePath : RowItem->DisplayName);
						})
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.ToolTipText_Lambda([RowItem = Item]()
						{
							return FText::FromString(BuildTooltip(*RowItem));
						})
					];
			}
			if (ColumnName == OwnerColumn)
			{
				return SNew(STextBlock)
					.Text_Lambda([RowItem = Item]()
					{
						return FText::FromString(RowItem->DisplayOwnerLevel.IsEmpty() ? GetOwnerLevelDisplayText(RowItem->OwnerLevel) : RowItem->DisplayOwnerLevel);
					})
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ToolTipText_Lambda([RowItem = Item]()
					{
						return FText::FromString(RowItem->OwnerLevel.IsEmpty() ? TEXT("No owner level") : RowItem->OwnerLevel);
					});
			}
			if (ColumnName == PathColumn)
			{
				return SNew(STextBlock)
					.Text_Lambda([RowItem = Item]()
					{
						return FText::FromString(GetObjectPathDisplayText(*RowItem));
					})
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ToolTipText_Lambda([RowItem = Item]()
					{
						const FString FullPath = RowItem->ObjectPath.IsEmpty() ? RowItem->RepositoryRelativePath : RowItem->ObjectPath;
						const FString PathTooltip = RowItem->FullDataLayerNames.IsEmpty()
							? FullPath
							: FString::Printf(TEXT("%s\nData Layers: %s"), *FullPath, *RowItem->FullDataLayerNames);
						return FText::FromString(PathTooltip);
					});
			}
			if (ColumnName == TypeColumn)
			{
				return SNew(STextBlock)
					.Text_Lambda([RowItem = Item]()
					{
						return FText::FromString(RowItem->AssetType.IsEmpty() ? GetPackageKindText(RowItem->PackageKind) : RowItem->AssetType);
					})
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ToolTipText_Lambda([RowItem = Item]()
					{
						const FString Type = RowItem->AssetType.IsEmpty() ? GetPackageKindText(RowItem->PackageKind) : RowItem->AssetType;
						return FText::FromString(Type);
					});
			}
			if (ColumnName == StatusColumn)
			{
				return SNew(STextBlock)
					.Text_Lambda([RowItem = Item]()
					{
						return FText::FromString(LexToString(RowItem->State));
					})
					.ColorAndOpacity_Lambda([RowItem = Item]()
					{
						return GetStateColor(RowItem->State);
					})
					.ToolTipText_Lambda([RowItem = Item]()
					{
						return FText::FromString(RowItem->bCanRevert ? TEXT("Revertable") : RowItem->RevertBlockReason);
					});
			}

			return SNew(STextBlock).Text(FText::GetEmpty());
		}

		virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
		{
			if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				if (const TSharedPtr<SGitChangedAssetsPanel> PinnedPanel = Panel.Pin())
				{
					return PinnedPanel->HandleEntryMouseButtonDown(Item, MouseEvent);
				}
			}

			return SMultiColumnTableRow<TSharedPtr<FGitChangedAssetEntry>>::OnMouseButtonDown(MyGeometry, MouseEvent);
		}

	private:
		TSharedPtr<FGitChangedAssetEntry> Item;
		TWeakPtr<SGitChangedAssetsPanel> Panel;
	};

	TSharedPtr<FString> FindOrAddOption(TArray<TSharedPtr<FString>>& Options, const FString& Value)
	{
		if (const TSharedPtr<FString>* Existing = Options.FindByPredicate([&Value](const TSharedPtr<FString>& Candidate)
		{
			return Candidate.IsValid() && Candidate->Equals(Value, ESearchCase::CaseSensitive);
		}))
		{
			return *Existing;
		}
		return Options.Add_GetRef(MakeShared<FString>(Value));
	}

	bool ShouldRequestInitialRefresh(const bool bAlreadyRequested, const bool bGitCapabilityAvailable)
	{
		return !bAlreadyRequested && bGitCapabilityAvailable;
	}
}

void SGitChangedAssetsPanel::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	check(Controller.IsValid());

	using namespace SGitChangedAssetsPanelPrivate;
	SelectedStatus = FindOrAddOption(StatusOptions, TEXT("All statuses"));
	FindOrAddOption(StatusOptions, TEXT("Modified"));
	FindOrAddOption(StatusOptions, TEXT("Deleted"));
	FindOrAddOption(StatusOptions, TEXT("Added"));
	FindOrAddOption(StatusOptions, TEXT("Untracked"));
	FindOrAddOption(StatusOptions, TEXT("Renamed"));
	FindOrAddOption(StatusOptions, TEXT("Conflicted"));
	SelectedKind = FindOrAddOption(KindOptions, TEXT("All kinds"));
	FindOrAddOption(KindOptions, TEXT("Regular asset"));
	FindOrAddOption(KindOptions, TEXT("External actor"));
	FindOrAddOption(KindOptions, TEXT("External object"));
	FindOrAddOption(KindOptions, TEXT("Unknown"));
	SelectedOwner = FindOrAddOption(OwnerOptions, TEXT("All owner levels"));
	SelectedRevertable = FindOrAddOption(RevertableOptions, TEXT("All entries"));
	FindOrAddOption(RevertableOptions, TEXT("Revertable"));
	FindOrAddOption(RevertableOptions, TEXT("Blocked"));

	ControllerRowsChangedHandle = Controller->OnRowsChanged().AddSP(SharedThis(this), &SGitChangedAssetsPanel::HandleControllerRowsChanged);
	ControllerActivityChangedHandle = Controller->OnActivityChanged().AddSP(SharedThis(this), &SGitChangedAssetsPanel::HandleControllerActivityChanged);

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.Padding(8.0f)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.IsEnabled(this, &SGitChangedAssetsPanel::IsContentEnabled)
			[
				SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("ChangedAssetsSearchHint", "Search name, owner level, path, or type"))
					.OnTextChanged(this, &SGitChangedAssetsPanel::HandleSearchChanged)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SAssignNew(StatusComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&StatusOptions)
					.InitiallySelectedItem(SelectedStatus)
					.OnGenerateWidget(this, &SGitChangedAssetsPanel::GenerateFilterOption)
					.OnSelectionChanged(this, &SGitChangedAssetsPanel::HandleStatusChanged)
					[
						SNew(STextBlock).Text_Lambda([this]() { return SelectedStatus.IsValid() ? FText::FromString(*SelectedStatus) : FText::GetEmpty(); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SAssignNew(KindComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&KindOptions)
					.InitiallySelectedItem(SelectedKind)
					.OnGenerateWidget(this, &SGitChangedAssetsPanel::GenerateFilterOption)
					.OnSelectionChanged(this, &SGitChangedAssetsPanel::HandleKindChanged)
					[
						SNew(STextBlock).Text_Lambda([this]() { return SelectedKind.IsValid() ? FText::FromString(*SelectedKind) : FText::GetEmpty(); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SAssignNew(OwnerComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&OwnerOptions)
					.InitiallySelectedItem(SelectedOwner)
					.OnGenerateWidget(this, &SGitChangedAssetsPanel::GenerateFilterOption)
					.OnSelectionChanged(this, &SGitChangedAssetsPanel::HandleOwnerChanged)
					[
						SNew(STextBlock).Text_Lambda([this]() { return SelectedOwner.IsValid() ? FText::FromString(*SelectedOwner) : FText::GetEmpty(); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SAssignNew(RevertableComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&RevertableOptions)
					.InitiallySelectedItem(SelectedRevertable)
					.OnGenerateWidget(this, &SGitChangedAssetsPanel::GenerateFilterOption)
					.OnSelectionChanged(this, &SGitChangedAssetsPanel::HandleRevertableChanged)
					[
						SNew(STextBlock).Text_Lambda([this]() { return SelectedRevertable.IsValid() ? FText::FromString(*SelectedRevertable) : FText::GetEmpty(); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(this, &SGitChangedAssetsPanel::GetCopyPathsButtonText)
					.IsEnabled(this, &SGitChangedAssetsPanel::HasSelection)
					.OnClicked(this, &SGitChangedAssetsPanel::HandleCopyPathsClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ChangedAssetsRefresh", "Refresh"))
					.IsEnabled_Lambda([this]() { return Controller.IsValid() && !Controller->IsRefreshing() && !Controller->IsReverting(); })
					.OnClicked(this, &SGitChangedAssetsPanel::HandleRefreshClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(this, &SGitChangedAssetsPanel::GetRevertButtonText)
					.IsEnabled_Lambda([this]() { return CanRevertSelection(); })
					.OnClicked(this, &SGitChangedAssetsPanel::HandleRevertClicked)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 0.0f, 2.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(this, &SGitChangedAssetsPanel::GetStatusText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f, 0.0f, 2.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(this, &SGitChangedAssetsPanel::GetErrorText)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.28f, 0.28f)))
				.Visibility(this, &SGitChangedAssetsPanel::GetErrorVisibility)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(ListView, SChangedAssetsListView, SharedThis(this))
				.ListItemsSource(&FilteredItems)
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SGitChangedAssetsPanel::GenerateRow)
				.OnSelectionChanged(this, &SGitChangedAssetsPanel::HandleListSelectionChanged)
				.HeaderRow
				(
					SNew(SHeaderRow)
					+ SHeaderRow::Column(NameColumn).DefaultLabel(LOCTEXT("ChangedAssetsName", "Name")).FillWidth(0.21f)
					+ SHeaderRow::Column(OwnerColumn).DefaultLabel(LOCTEXT("ChangedAssetsOwner", "Owner Level")).FillWidth(0.20f)
					+ SHeaderRow::Column(PathColumn).DefaultLabel(LOCTEXT("ChangedAssetsPath", "Asset or Object Path")).FillWidth(0.38f)
					+ SHeaderRow::Column(TypeColumn).DefaultLabel(LOCTEXT("ChangedAssetsType", "Type")).FillWidth(0.13f)
					+ SHeaderRow::Column(StatusColumn).DefaultLabel(LOCTEXT("ChangedAssetsStatus", "Status")).FillWidth(0.08f)
				)
			]
			]
		]
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.Visibility(this, &SGitChangedAssetsPanel::GetBusyOverlayVisibility)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.70f))
			[
				SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.Padding(FMargin(20.0f, 14.0f))
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.BorderBackgroundColor(FLinearColor(0.06f, 0.06f, 0.06f, 0.96f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 10.0f, 0.0f)
						[
							SNew(SCircularThrobber)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SGitChangedAssetsPanel::GetStatusText)
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]
			]
		]
	];

	HandleControllerRowsChanged();
}

SGitChangedAssetsPanel::~SGitChangedAssetsPanel()
{
	if (Controller.IsValid() && ControllerRowsChangedHandle.IsValid())
	{
		Controller->OnRowsChanged().Remove(ControllerRowsChangedHandle);
	}
	if (Controller.IsValid() && ControllerActivityChangedHandle.IsValid())
	{
		Controller->OnActivityChanged().Remove(ControllerActivityChangedHandle);
	}
}

void SGitChangedAssetsPanel::HandleControllerRowsChanged()
{
	RebuildItems();
	RequestInitialRefreshIfAvailable();
}

void SGitChangedAssetsPanel::HandleControllerActivityChanged()
{
	// 活动状态只更新控件属性和忙碌遮罩, 不重建虚拟化行模型.
	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
	RequestInitialRefreshIfAvailable();
}

void SGitChangedAssetsPanel::RequestInitialRefreshIfAvailable()
{
	if (Controller.IsValid() && SGitChangedAssetsPanelPrivate::ShouldRequestInitialRefresh(bInitialRefreshRequested, IsStartupGitCapabilityAvailable()))
	{
		bInitialRefreshRequested = true;
		Controller->Refresh();
	}
}

void SGitChangedAssetsPanel::RebuildItems()
{
	uint64 IncomingGeneration = MAX_uint64;
	const FGitChangedAssetSnapshot* Snapshot = nullptr;
	if (Controller.IsValid())
	{
		Snapshot = Controller->GetSnapshot();
		if (Snapshot != nullptr)
		{
			IncomingGeneration = Snapshot->Generation;
		}
	}
	if (IncomingGeneration != DisplayedSnapshotGeneration)
	{
		ClearListSelection();
		ItemsByAbsoluteFilename.Reset();
		DisplayedSnapshotGeneration = IncomingGeneration;
	}
	if (Snapshot != nullptr)
	{
		SGitChangedAssetsPanelPrivate::ReconcileItemsByIdentity(Snapshot->Entries, ItemsByAbsoluteFilename, AllItems);
	}
	else
	{
		AllItems.Reset();
	}

	ApplyFilters(RebuildOwnerOptions());
}

bool SGitChangedAssetsPanel::RebuildOwnerOptions()
{
	using namespace SGitChangedAssetsPanelPrivate;
	FOwnerOptionsBuildResult RebuiltOptions = BuildOwnerOptions(OwnerOptions, SelectedOwner, AllItems);
	OwnerOptions = MoveTemp(RebuiltOptions.Options);
	SelectedOwner = MoveTemp(RebuiltOptions.SelectedOption);
	if (OwnerComboBox.IsValid())
	{
		TGuardValue<bool> UpdatingOwnerOptionsGuard(bUpdatingOwnerOptions, true);
		OwnerComboBox->RefreshOptions();
		OwnerComboBox->SetSelectedItem(SelectedOwner);
	}
	return RebuiltOptions.bOwnerFilterChanged;
}

void SGitChangedAssetsPanel::ApplyFilters(const bool bClearSelection)
{
	if (bClearSelection)
	{
		ClearListSelection();
	}

	FilteredItems.Reset();
	for (const FEntryPtr& Item : AllItems)
	{
		if (Item.IsValid() && MatchesFilters(*Item))
		{
			FilteredItems.Add(Item);
		}
	}

	FilteredItems.Sort([](const FEntryPtr& Left, const FEntryPtr& Right)
	{
		const FString LeftOwner = Left.IsValid() ? Left->OwnerLevel : FString();
		const FString RightOwner = Right.IsValid() ? Right->OwnerLevel : FString();
		const int32 OwnerComparison = LeftOwner.Compare(RightOwner, ESearchCase::IgnoreCase);
		if (OwnerComparison != 0)
		{
			return OwnerComparison < 0;
		}
		const FString LeftName = Left.IsValid() ? Left->DisplayName : FString();
		const FString RightName = Right.IsValid() ? Right->DisplayName : FString();
		return LeftName.Compare(RightName, ESearchCase::IgnoreCase) < 0;
	});
	PruneListSelectionToFilteredItems();

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

void SGitChangedAssetsPanel::ClearListSelection()
{
	SelectionAnchor.Reset();
	if (ListView.IsValid())
	{
		ListView->ClearSelection();
		RebaseListNavigationState(nullptr);
	}
}

void SGitChangedAssetsPanel::PruneListSelectionToFilteredItems()
{
	if (!ListView.IsValid())
	{
		return;
	}

	const TArray<FEntryPtr> CurrentSelection = GetSelectedEntries();
	for (const FEntryPtr& Item : CurrentSelection)
	{
		if (!SGitChangedAssetsPanelPrivate::ContainsEntry(FilteredItems, Item))
		{
			ListView->SetItemSelection(Item, false, ESelectInfo::Direct);
		}
	}
	SelectionAnchor = SGitChangedAssetsPanelPrivate::ResolveSelectionAnchor(FilteredItems, GetSelectedEntries(), GetListRangeSelectionAnchor());
	RebaseListNavigationState(SelectionAnchor);
}

void SGitChangedAssetsPanel::ReplaceListSelection(const TArray<FEntryPtr>& NewSelection, const FEntryPtr& UserDirectedItem)
{
	if (!ListView.IsValid())
	{
		return;
	}

	{
		TGuardValue<bool> ApplyingListSelectionGuard(bApplyingListSelection, true);
		ListView->ClearSelection();
		if (!NewSelection.IsEmpty())
		{
			ListView->SetItemSelection(NewSelection, true, ESelectInfo::Direct);
		}

		// Keep Slate's keyboard-selection anchor aligned with mouse toggle clicks. Shift range
		// clicks intentionally retain the previous anchor, matching the panel's range semantics.
		if (UserDirectedItem.IsValid())
		{
			const bool bShouldBeSelected = NewSelection.ContainsByPredicate([&UserDirectedItem](const FEntryPtr& Item)
			{
				return Item == UserDirectedItem;
			});
			ListView->SetItemSelection(UserDirectedItem, bShouldBeSelected, ESelectInfo::OnMouseClick);
		}
	}

	ListView->RequestListRefresh();
}

TArray<SGitChangedAssetsPanel::FEntryPtr> SGitChangedAssetsPanel::GetSelectedEntries() const
{
	return ListView.IsValid() ? ListView->GetSelectedItems() : TArray<FEntryPtr>();
}


SGitChangedAssetsPanel::FEntryPtr SGitChangedAssetsPanel::GetListRangeSelectionAnchor() const
{
	if (!ListView.IsValid())
	{
		return nullptr;
	}
	return StaticCastSharedPtr<SChangedAssetsListView>(ListView)->GetRangeSelectionAnchor();
}

void SGitChangedAssetsPanel::RebaseListNavigationState(const FEntryPtr& NewAnchor)
{
	if (ListView.IsValid())
	{
		StaticCastSharedPtr<SChangedAssetsListView>(ListView)->RebaseNavigationState(NewAnchor);
	}
}

void SGitChangedAssetsPanel::HandleListSelectionChanged(FEntryPtr Item, ESelectInfo::Type SelectInfo)
{
	(void)Item;
	(void)SelectInfo;
	if (!bApplyingListSelection)
	{
		SelectionAnchor = SGitChangedAssetsPanelPrivate::ResolveSelectionAnchor(FilteredItems, GetSelectedEntries(), GetListRangeSelectionAnchor());
	}
}

bool SGitChangedAssetsPanel::MatchesFilters(const FGitChangedAssetEntry& Entry) const
{
	using namespace SGitChangedAssetsPanelPrivate;
	if (SelectedStatus.IsValid() && !SelectedStatus->Equals(TEXT("All statuses"), ESearchCase::CaseSensitive)
		&& !SelectedStatus->Equals(LexToString(Entry.State), ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (SelectedKind.IsValid() && !SelectedKind->Equals(TEXT("All kinds"), ESearchCase::CaseSensitive)
		&& !SelectedKind->Equals(GetPackageKindText(Entry.PackageKind), ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (SelectedOwner.IsValid() && !SelectedOwner->Equals(TEXT("All owner levels"), ESearchCase::CaseSensitive))
	{
		if (SelectedOwner->Equals(TEXT("<No owner level>"), ESearchCase::CaseSensitive))
		{
			if (!Entry.OwnerLevel.IsEmpty())
			{
				return false;
			}
		}
		else if (!SelectedOwner->Equals(Entry.OwnerLevel, ESearchCase::CaseSensitive))
		{
			return false;
		}
	}
	if (SelectedRevertable.IsValid())
	{
		if (SelectedRevertable->Equals(TEXT("Revertable"), ESearchCase::CaseSensitive) && !Entry.bCanRevert)
		{
			return false;
		}
		if (SelectedRevertable->Equals(TEXT("Blocked"), ESearchCase::CaseSensitive) && Entry.bCanRevert)
		{
			return false;
		}
	}
	if (!SearchText.IsEmpty())
	{
		const bool bMatches = Entry.DisplayName.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.OwnerLevel.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.DisplayOwnerLevel.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.ObjectPath.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.DisplayObjectPath.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.FullDataLayerNames.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.AssetType.Contains(SearchText, ESearchCase::IgnoreCase)
			|| Entry.RepositoryRelativePath.Contains(SearchText, ESearchCase::IgnoreCase);
		if (!bMatches)
		{
			return false;
		}
	}
	return true;
}

TSharedRef<ITableRow> SGitChangedAssetsPanel::GenerateRow(FEntryPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SGitChangedAssetsPanelPrivate::SChangedAssetRow, OwnerTable)
		.Item(Item)
		.Panel(SharedThis(this));
}

void SGitChangedAssetsPanel::HandleSearchChanged(const FText& InText)
{
	SearchText = InText.ToString();
	ApplyFilters(true);
}

void SGitChangedAssetsPanel::HandleStatusChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	(void)SelectInfo;
	SelectedStatus = MoveTemp(NewSelection);
	ApplyFilters(true);
}

void SGitChangedAssetsPanel::HandleKindChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	(void)SelectInfo;
	SelectedKind = MoveTemp(NewSelection);
	ApplyFilters(true);
}

void SGitChangedAssetsPanel::HandleOwnerChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	(void)SelectInfo;
	if (bUpdatingOwnerOptions)
	{
		return;
	}
	const FString PreviousOwner = SelectedOwner.IsValid() ? *SelectedOwner : TEXT("All owner levels");
	const FString NextOwner = NewSelection.IsValid() ? *NewSelection : TEXT("All owner levels");
	if (PreviousOwner.Equals(NextOwner, ESearchCase::CaseSensitive))
	{
		SelectedOwner = MoveTemp(NewSelection);
		return;
	}
	SelectedOwner = MoveTemp(NewSelection);
	ApplyFilters(true);
}

void SGitChangedAssetsPanel::HandleRevertableChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	(void)SelectInfo;
	SelectedRevertable = MoveTemp(NewSelection);
	ApplyFilters(true);
}

TSharedRef<SWidget> SGitChangedAssetsPanel::GenerateFilterOption(TSharedPtr<FString> Option) const
{
	return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString()));
}

FReply SGitChangedAssetsPanel::HandleRefreshClicked()
{
	if (IsStartupGitCapabilityAvailable() && Controller.IsValid())
	{
		Controller->Refresh();
	}
	return FReply::Handled();
}

bool SGitChangedAssetsPanel::HasSelection() const
{
	return IsStartupGitCapabilityAvailable() && ListView.IsValid() && ListView->GetNumItemsSelected() > 0;
}

FReply SGitChangedAssetsPanel::HandleCopyPathsClicked()
{
	if (!HasSelection())
	{
		return FReply::Unhandled();
	}

	const TArray<FString> Paths = SGitChangedAssetsPanelPrivate::BuildSelectedAbsolutePaths(FilteredItems, GetSelectedEntries());
	if (!Paths.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Paths, TEXT("\n")));
	}
	return FReply::Handled();
}

FReply SGitChangedAssetsPanel::HandleRevertClicked()
{
	if (!Controller.IsValid() || !CanRevertSelection())
	{
		return FReply::Unhandled();
	}

	TArray<FGitChangedAssetEntry> EntriesToRevert;
	for (const FEntryPtr& Item : GetSelectedEntries())
	{
		if (!Item.IsValid())
		{
			continue;
		}
		EntriesToRevert.Add(*Item);
	}
	if (EntriesToRevert.IsEmpty())
	{
		return FReply::Unhandled();
	}

	Controller->RevertToHead(MoveTemp(EntriesToRevert));
	return FReply::Handled();
}

bool SGitChangedAssetsPanel::CanRevertSelection() const
{
	if (!IsStartupGitCapabilityAvailable() || !Controller.IsValid() || Controller->IsRefreshing() || Controller->IsReverting())
	{
		return false;
	}
	const TArray<FEntryPtr> SelectedEntries = GetSelectedEntries();
	if (SelectedEntries.IsEmpty())
	{
		return false;
	}
	for (const FEntryPtr& Item : SelectedEntries)
	{
		if (!Item.IsValid() || !Item->bCanRevert)
		{
			return false;
		}
	}
	return true;
}

bool SGitChangedAssetsPanel::IsStartupGitCapabilityAvailable() const
{
	return GitSourceControlUtils::IsStartupGitCapabilityAvailable();
}

bool SGitChangedAssetsPanel::IsPanelBusy() const
{
	return Controller.IsValid() && (Controller->IsRefreshing() || Controller->IsReverting());
}

bool SGitChangedAssetsPanel::IsContentEnabled() const
{
	return IsStartupGitCapabilityAvailable() && !IsPanelBusy();
}

EVisibility SGitChangedAssetsPanel::GetBusyOverlayVisibility() const
{
	return IsPanelBusy() ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SGitChangedAssetsPanel::HandleEntryMouseButtonDown(FEntryPtr Entry, const FPointerEvent& MouseEvent)
{
	if (!IsStartupGitCapabilityAvailable() || !Entry.IsValid() || !ListView.IsValid() || MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	const bool bShiftDown = MouseEvent.IsShiftDown();
	const bool bControlDown = MouseEvent.IsControlDown();
	const TArray<FEntryPtr> CurrentSelection = GetSelectedEntries();
	const bool bHasRangeAnchor = SelectionAnchor.IsValid()
		&& SGitChangedAssetsPanelPrivate::ContainsEntry(FilteredItems, SelectionAnchor);
	const TArray<FEntryPtr> NewSelection = SGitChangedAssetsPanelPrivate::BuildMouseSelection(
		FilteredItems, CurrentSelection, bHasRangeAnchor ? SelectionAnchor : FEntryPtr(), Entry, bShiftDown, bControlDown);
	ReplaceListSelection(NewSelection, bShiftDown && bHasRangeAnchor ? FEntryPtr() : Entry);
	if (!bShiftDown || !bHasRangeAnchor)
	{
		SelectionAnchor = Entry;
	}

	return FReply::Handled().SetUserFocus(ListView.ToSharedRef(), EFocusCause::Mouse);
}

void SGitChangedAssetsPanel::HandleListBlankMouseButtonDown()
{
	SelectionAnchor.Reset();
	RebaseListNavigationState(nullptr);
}

ECheckBoxState SGitChangedAssetsPanel::GetEntryCheckState(FEntryPtr Entry) const
{
	return Entry.IsValid() && ListView.IsValid() && ListView->IsItemSelected(Entry) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

FText SGitChangedAssetsPanel::GetStatusText() const
{
	if (!IsStartupGitCapabilityAvailable())
	{
		return GitSourceControlUtils::GetStartupGitCapabilityMessage();
	}
	if (!Controller.IsValid())
	{
		return LOCTEXT("ChangedAssetsControllerUnavailable", "Git Changes is unavailable.");
	}
	if (Controller->IsReverting())
	{
		return LOCTEXT("ChangedAssetsReverting", "Reverting selected assets to the pinned HEAD...");
	}
	if (Controller->IsRefreshing())
	{
		const int32 Completed = Controller->GetRefreshProgressCompleted();
		const int32 Total = Controller->GetRefreshProgressTotal();
		auto FormatProgress = [Completed, Total](const FText& InLabel)
		{
			return Total > 0
				? FText::Format(LOCTEXT("ChangedAssetsRefreshProgress", "{0} ({1}/{2})..."), InLabel, FText::AsNumber(Completed), FText::AsNumber(Total))
				: FText::Format(LOCTEXT("ChangedAssetsRefreshWorking", "{0}..."), InLabel);
		};
		switch (Controller->GetRefreshPhase())
		{
		case EGitChangedAssetsRefreshPhase::GitStatus:
			return FormatProgress(LOCTEXT("ChangedAssetsRefreshingGitStatus", "Refreshing repository Git status"));
		case EGitChangedAssetsRefreshPhase::CurrentMetadata:
			return FormatProgress(LOCTEXT("ChangedAssetsRefreshingCurrentMetadata", "Reading current asset metadata"));
		case EGitChangedAssetsRefreshPhase::HeadMetadata:
			return FormatProgress(LOCTEXT("ChangedAssetsRefreshingHeadMetadata", "Reading HEAD asset metadata"));
		case EGitChangedAssetsRefreshPhase::OwnerFallback:
			return FormatProgress(LOCTEXT("ChangedAssetsRefreshingOwnerFallback", "Resolving owner levels"));
		default:
			return LOCTEXT("ChangedAssetsRefreshing", "Refreshing Git Changes...");
		}
	}
	if (const FGitChangedAssetSnapshot* Snapshot = Controller->GetSnapshot())
	{
		return FText::Format(LOCTEXT("ChangedAssetsSnapshotStatus", "{0} changed .uasset entries. Snapshot {1}, Git status {2}s."),
			FText::AsNumber(Snapshot->Entries.Num()),
			FText::FromString(Snapshot->CapturedAtUtc.ToString()),
			FText::AsNumber(Snapshot->StatusDurationSeconds));
	}
	return LOCTEXT("ChangedAssetsNoSnapshot", "No successful Git Changes snapshot yet.");
}

FText SGitChangedAssetsPanel::GetErrorText() const
{
	return !IsStartupGitCapabilityAvailable()
		? GitSourceControlUtils::GetStartupGitCapabilityMessage()
		: Controller.IsValid() ? FText::FromString(Controller->GetLastError()) : FText::GetEmpty();
}

EVisibility SGitChangedAssetsPanel::GetErrorVisibility() const
{
	return !IsStartupGitCapabilityAvailable() || (Controller.IsValid() && !Controller->GetLastError().IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SGitChangedAssetsPanel::GetRevertButtonText() const
{
	const int32 SelectedCount = ListView.IsValid() ? ListView->GetNumItemsSelected() : 0;
	return SelectedCount == 0
		? LOCTEXT("ChangedAssetsRevert", "Revert Selected to HEAD...")
		: FText::Format(LOCTEXT("ChangedAssetsRevertCount", "Revert {0} to HEAD..."), FText::AsNumber(SelectedCount));
}

FText SGitChangedAssetsPanel::GetCopyPathsButtonText() const
{
	const int32 SelectedCount = ListView.IsValid() ? ListView->GetNumItemsSelected() : 0;
	return SelectedCount <= 1
		? LOCTEXT("ChangedAssetsCopyPath", "Copy File Path")
		: FText::Format(LOCTEXT("ChangedAssetsCopyPaths", "Copy {0} File Paths"), FText::AsNumber(SelectedCount));
}


#undef LOCTEXT_NAMESPACE
