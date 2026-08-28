// Copyright (c) 2026

#include "SGitStandaloneHistoryWindow.h"

#include "GitSourceControlUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace SGitStandaloneHistoryWindowPrivate
{
	using FRevisionPtr = TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe>;

	struct FHistoryTreeItem
	{
		FString Filename;
		FRevisionPtr Revision;
		TArray<TSharedPtr<FHistoryTreeItem>> Children;

		bool IsFileNode() const
		{
			return !Revision.IsValid();
		}
	};

	using FHistoryTreeItemPtr = TSharedPtr<FHistoryTreeItem>;

	struct FHistoryDiffRequest
	{
		FRevisionPtr OlderRevision;
		FRevisionPtr NewerRevision;
		bool bAgainstWorkspace = false;
	};

	bool IsUsableRevision(const FRevisionPtr& Revision)
	{
		return Revision.IsValid() && !Revision->CommitId.IsEmpty() && !Revision->Filename.IsEmpty();
	}

	bool HasRevisionBlob(const FRevisionPtr& Revision)
	{
		if (!IsUsableRevision(Revision))
		{
			return false;
		}

		FString NormalizedAction = Revision->Action;
		NormalizedAction.TrimStartAndEndInline();
		return !NormalizedAction.Equals(TEXT("d"), ESearchCase::IgnoreCase)
			&& !NormalizedAction.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
			&& !NormalizedAction.Equals(TEXT("deleted"), ESearchCase::IgnoreCase)
			&& !NormalizedAction.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
			&& !NormalizedAction.Equals(TEXT("removed"), ESearchCase::IgnoreCase);
	}

	TOptional<FHistoryDiffRequest> MakeWorkspaceDiffRequest(const FRevisionPtr& Revision)
	{
		if (!HasRevisionBlob(Revision))
		{
			return {};
		}
		FHistoryDiffRequest Request;
		Request.OlderRevision = Revision;
		Request.bAgainstWorkspace = true;
		return Request;
	}

	TOptional<FHistoryDiffRequest> MakePreviousDiffRequest(const TArray<FRevisionPtr>& Revisions, const FRevisionPtr& NewerRevision)
	{
		const int32 NewerIndex = Revisions.IndexOfByKey(NewerRevision);
		if (!HasRevisionBlob(NewerRevision) || NewerIndex == INDEX_NONE || !Revisions.IsValidIndex(NewerIndex + 1) || !HasRevisionBlob(Revisions[NewerIndex + 1]))
		{
			return {};
		}
		FHistoryDiffRequest Request;
		Request.OlderRevision = Revisions[NewerIndex + 1];
		Request.NewerRevision = NewerRevision;
		return Request;
	}

	TOptional<FHistoryDiffRequest> MakeSelectedRevisionDiffRequest(const TArray<FRevisionPtr>& Revisions, const TArray<FRevisionPtr>& Selected)
	{
		if (Selected.Num() != 2 || !HasRevisionBlob(Selected[0]) || !HasRevisionBlob(Selected[1]) || Selected[0] == Selected[1])
		{
			return {};
		}
		const int32 FirstIndex = Revisions.IndexOfByKey(Selected[0]);
		const int32 SecondIndex = Revisions.IndexOfByKey(Selected[1]);
		if (FirstIndex == INDEX_NONE || SecondIndex == INDEX_NONE || FirstIndex == SecondIndex)
		{
			return {};
		}
		FHistoryDiffRequest Request;
		Request.OlderRevision = FirstIndex > SecondIndex ? Selected[0] : Selected[1];
		Request.NewerRevision = FirstIndex > SecondIndex ? Selected[1] : Selected[0];
		return Request;
	}

	FText GetModeLabel(const EGitLocalSourceControlHistoryMode Mode)
	{
		return Mode == EGitLocalSourceControlHistoryMode::ExactRenames
			? LOCTEXT("StandaloneHistoryExactRenames", "Exact Renames")
			: LOCTEXT("StandaloneHistoryCurrentPath", "Current Path");
	}

	FText GetDateText(const FGitSourceControlRevision& Revision)
	{
		return Revision.Date > FDateTime::MinValue() ? FText::AsDateTime(Revision.Date) : FText::GetEmpty();
	}

	FText GetSingleLineDescription(const FString& Description)
	{
		FString FirstLine;
		FString IgnoredRemainder;
		if (!Description.Split(TEXT("\n"), &FirstLine, &IgnoredRemainder))
		{
			FirstLine = Description;
		}
		FirstLine.TrimStartAndEndInline();
		return FText::FromString(MoveTemp(FirstLine));
	}

	const FSlateBrush* GetActionBrush(const FString& Action)
	{
		const FString LowerAction = Action.ToLower();
		if (LowerAction == TEXT("add"))
		{
			return FAppStyle::GetBrush(TEXT("SourceControl.Add"));
		}
		if (LowerAction == TEXT("d") || LowerAction == TEXT("delete") || LowerAction == TEXT("deleted") || LowerAction == TEXT("remove") || LowerAction == TEXT("removed"))
		{
			return FAppStyle::GetBrush(TEXT("SourceControl.Delete"));
		}
		if (LowerAction.Contains(TEXT("branch")))
		{
			return FAppStyle::GetBrush(TEXT("SourceControl.Branch"));
		}
		if (LowerAction.Contains(TEXT("integrate")) || LowerAction.Contains(TEXT("merge")))
		{
			return FAppStyle::GetBrush(TEXT("SourceControl.Integrate"));
		}
		return FAppStyle::GetBrush(TEXT("SourceControl.Edit"));
	}

	class SGitHistoryFileRow final : public STableRow<FHistoryTreeItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitHistoryFileRow) {}
			SLATE_ARGUMENT(FHistoryTreeItemPtr, Item)
			SLATE_ARGUMENT(EGitLocalSourceControlHistoryMode, Mode)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView)
		{
			Item = InArgs._Item;
			const EGitLocalSourceControlHistoryMode Mode = InArgs._Mode;
			STableRow<FHistoryTreeItemPtr>::Construct(
				STableRow<FHistoryTreeItemPtr>::FArguments()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SExpanderArrow, SharedThis(this))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 3.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle(TEXT("BoldFont")))
						.Text(FText::FromString(Item.IsValid() ? Item->Filename : FString()))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 3.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(GetModeLabel(Mode))
					]
				],
				InOwnerTableView);
		}

	private:
		FHistoryTreeItemPtr Item;
	};

	class SGitHistoryRevisionRow final : public SMultiColumnTableRow<FHistoryTreeItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitHistoryRevisionRow) {}
			SLATE_ARGUMENT(FHistoryTreeItemPtr, Item)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView)
		{
			Item = InArgs._Item;
			SMultiColumnTableRow<FHistoryTreeItemPtr>::Construct(FSuperRowType::FArguments(), InOwnerTableView);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			const FRevisionPtr Revision = Item.IsValid() ? Item->Revision : nullptr;
			if (!Revision.IsValid())
			{
				return SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryMissingRevision", "Invalid revision"));
			}

			if (ColumnName == TEXT("Revision"))
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Fill)
					[
						SNew(SExpanderArrow, SharedThis(this))
						.Visibility(this, &SGitHistoryRevisionRow::GetExpanderVisibility)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(10.0f, 0.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(GetActionBrush(Revision->Action))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Revision->ShortCommitId))
					];
			}
			if (ColumnName == TEXT("Date"))
			{
				return SNew(STextBlock).Text(GetDateText(*Revision));
			}
			if (ColumnName == TEXT("UserName"))
			{
				return SNew(STextBlock).Text(FText::FromString(Revision->UserName));
			}
			if (ColumnName == TEXT("Description"))
			{
				return SNew(STextBlock)
					.Text(GetSingleLineDescription(Revision->Description))
					.ToolTipText(FText::FromString(Revision->Description))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
			}

			return SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryUnsupportedColumn", "Unsupported column"));
		}

	private:
		EVisibility GetExpanderVisibility() const
		{
			return Item.IsValid() && !Item->Children.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
		}

		FHistoryTreeItemPtr Item;
	};

	class SGitStandaloneHistoryWindow final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGitStandaloneHistoryWindow) {}
			SLATE_ARGUMENT(FString, Filename)
			SLATE_ARGUMENT(EGitLocalSourceControlHistoryMode, Mode)
			SLATE_ARGUMENT(TGitSourceControlHistory, History)
			SLATE_EVENT(FGitStandaloneHistoryRestoreDelegate, OnRestore)
			SLATE_EVENT(FGitStandaloneHistoryRefreshDelegate, OnRefresh)
			SLATE_EVENT(FGitStandaloneHistoryDiffDelegate, OnDiff)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Filename = InArgs._Filename;
			Mode = InArgs._Mode;
			OnRestore = InArgs._OnRestore;
			OnRefresh = InArgs._OnRefresh;
			OnDiff = InArgs._OnDiff;

			const FHistoryTreeItemPtr FileItem = MakeShared<FHistoryTreeItem>();
			FileItem->Filename = Filename;
			Revisions.Reserve(InArgs._History.Num());
			FileItem->Children.Reserve(InArgs._History.Num());
			for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : InArgs._History)
			{
				const FRevisionPtr RevisionPtr = Revision;
				Revisions.Add(RevisionPtr);
				const FHistoryTreeItemPtr RevisionItem = MakeShared<FHistoryTreeItem>();
				RevisionItem->Revision = RevisionPtr;
				FileItem->Children.Add(RevisionItem);
			}
			RootItems.Add(FileItem);

			const TSharedRef<SHeaderRow> HeaderRow = SNew(SHeaderRow)
				+ SHeaderRow::Column(TEXT("Revision"))
				.DefaultLabel(LOCTEXT("StandaloneHistoryRevisionColumn", "Revision"))
				.FillWidth(200.0f)
				+ SHeaderRow::Column(TEXT("Date"))
				.DefaultLabel(LOCTEXT("StandaloneHistoryDateColumn", "Date Submitted"))
				.FillWidth(250.0f)
				+ SHeaderRow::Column(TEXT("UserName"))
				.DefaultLabel(LOCTEXT("StandaloneHistoryUserNameColumn", "Submitted By"))
				.FillWidth(200.0f)
				+ SHeaderRow::Column(TEXT("Description"))
				.DefaultLabel(LOCTEXT("StandaloneHistoryDescriptionColumn", "Description"))
				.FillWidth(650.0f);

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
					.BorderBackgroundColor(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f))
					[
						SNew(SSplitter)
						.Orientation(Orient_Vertical)
						+ SSplitter::Slot()
						.Value(0.5f)
						[
							SNew(SBorder)
							[
								SAssignNew(HistoryTree, STreeView<FHistoryTreeItemPtr>)
								.TreeItemsSource(&RootItems)
								.SelectionMode(ESelectionMode::Multi)
								.OnSelectionChanged(this, &SGitStandaloneHistoryWindow::OnSelectionChanged)
								.OnGenerateRow(this, &SGitStandaloneHistoryWindow::GenerateHistoryRow)
								.OnGetChildren(this, &SGitStandaloneHistoryWindow::GetHistoryChildren)
								.HeaderRow(HeaderRow)
							]
						]
						+ SSplitter::Slot()
						.Value(0.5f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
							[
								BuildDetailsPanel()
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 6.0f)
				.HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(RevisionActionBar, SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("StandaloneHistoryDiffWorkspace", "Diff against Workspace"))
							.Visibility(this, &SGitStandaloneHistoryWindow::GetSingleRevisionActionVisibility)
							.OnClicked(this, &SGitStandaloneHistoryWindow::OnDiffAgainstWorkspace)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("StandaloneHistoryDiffPrevious", "Diff against Previous"))
							.Visibility(this, &SGitStandaloneHistoryWindow::GetPreviousRevisionActionVisibility)
							.OnClicked(this, &SGitStandaloneHistoryWindow::OnDiffAgainstPrevious)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("StandaloneHistoryDiffSelected", "Diff Selected Revisions"))
							.Visibility(this, &SGitStandaloneHistoryWindow::GetTwoRevisionActionVisibility)
							.OnClicked(this, &SGitStandaloneHistoryWindow::OnDiffSelectedRevisions)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("StandaloneHistoryRestore", "Restore Selected..."))
							.Visibility(EVisibility::Visible)
							.IsEnabled(this, &SGitStandaloneHistoryWindow::CanRestoreSelectedRevision)
							.ToolTipText(this, &SGitStandaloneHistoryWindow::GetRestoreTooltip)
							.OnClicked(this, &SGitStandaloneHistoryWindow::OnRestoreSelected)
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("StandaloneHistoryRefresh", "Refresh"))
						.OnClicked(this, &SGitStandaloneHistoryWindow::OnRefreshHistory)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("StandaloneHistoryClose", "Close"))
						.OnClicked(this, &SGitStandaloneHistoryWindow::OnClose)
					]
				]
			];

			HistoryTree->SetItemExpansion(FileItem, true);
		}

		virtual ~SGitStandaloneHistoryWindow() override
		{
			CancelActiveDiffs();
		}

	private:
		TSharedRef<SWidget> BuildDetailsPanel()
		{
			const FMargin FieldPadding(5.0f, 2.0f);
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(5.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoRevision", "Revision:"))]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoDate", "Date Submitted:"))]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoAuthor", "Submitted By:"))]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoAction", "Action:"))]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoPath", "Historical Path:"))]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(20.0f, 0.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(SEditableText).IsReadOnly(true).Text(this, &SGitStandaloneHistoryWindow::GetSelectedRevisionText)]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(SEditableText).IsReadOnly(true).Text(this, &SGitStandaloneHistoryWindow::GetSelectedDateText)]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(SEditableText).IsReadOnly(true).Text(this, &SGitStandaloneHistoryWindow::GetSelectedAuthorText)]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(SEditableText).IsReadOnly(true).Text(this, &SGitStandaloneHistoryWindow::GetSelectedActionText)]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FieldPadding)
						[SNew(SEditableText).IsReadOnly(true).Text(this, &SGitStandaloneHistoryWindow::GetSelectedPathText)]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(5.0f, 6.0f, 5.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("StandaloneHistoryInfoDescription", "Description:"))
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(5.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.AutoWrapText(true)
					.Text(this, &SGitStandaloneHistoryWindow::GetSelectedDescriptionText)
				];
		}

		TSharedRef<ITableRow> GenerateHistoryRow(FHistoryTreeItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
		{
			if (Item.IsValid() && Item->IsFileNode())
			{
				return SNew(SGitHistoryFileRow, OwnerTable).Item(Item).Mode(Mode);
			}
			return SNew(SGitHistoryRevisionRow, OwnerTable).Item(Item);
		}

		void GetHistoryChildren(FHistoryTreeItemPtr Item, TArray<FHistoryTreeItemPtr>& OutChildren) const
		{
			if (Item.IsValid())
			{
				OutChildren.Append(Item->Children);
			}
		}

		void OnSelectionChanged(FHistoryTreeItemPtr Item, ESelectInfo::Type)
		{
			LastSelectedRevision = Item.IsValid() ? Item->Revision : nullptr;
			if (RevisionActionBar.IsValid())
			{
				RevisionActionBar->Invalidate(EInvalidateWidgetReason::Layout);
			}
		}

		TArray<FRevisionPtr> GetSelectedRevisions() const
		{
			TArray<FRevisionPtr> SelectedRevisions;
			if (!HistoryTree.IsValid())
			{
				return SelectedRevisions;
			}
			for (const FHistoryTreeItemPtr& Item : HistoryTree->GetSelectedItems())
			{
				if (Item.IsValid() && Item->Revision.IsValid())
				{
					SelectedRevisions.Add(Item->Revision);
				}
			}
			return SelectedRevisions;
		}

		FRevisionPtr GetSingleSelectedRevision() const
		{
			const TArray<FRevisionPtr> Selected = GetSelectedRevisions();
			return Selected.Num() == 1 ? Selected[0] : nullptr;
		}

		bool IsSupportedCurrentAsset() const
		{
			return Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase);
		}

		bool HasCurrentHistoricalPath(const FRevisionPtr& Revision) const
		{
			// Historical paths are repository identities and must not collapse case-only renames.
			return !Revisions.IsEmpty() && HasRevisionBlob(Revision) && Revision->Filename.Equals(Revisions[0]->Filename, ESearchCase::CaseSensitive);
		}

		bool CanDiffAgainstWorkspace() const { return MakeWorkspaceDiffRequest(GetSingleSelectedRevision()).IsSet(); }
		bool CanDiffAgainstPrevious() const { return MakePreviousDiffRequest(Revisions, GetSingleSelectedRevision()).IsSet(); }
		bool CanDiffSelectedRevisions() const { return MakeSelectedRevisionDiffRequest(Revisions, GetSelectedRevisions()).IsSet(); }
		bool CanRestoreSelectedRevision() const { return IsSupportedCurrentAsset() && HasCurrentHistoricalPath(GetSingleSelectedRevision()); }
		FText GetRestoreTooltip() const
		{
			const TArray<FRevisionPtr> SelectedRevisions = GetSelectedRevisions();
			if (SelectedRevisions.IsEmpty())
			{
				return LOCTEXT("StandaloneHistoryRestoreNoSelection", "Select one history revision to restore.");
			}
			if (SelectedRevisions.Num() != 1)
			{
				return LOCTEXT("StandaloneHistoryRestoreSingleSelection", "Select exactly one history revision to restore.");
			}
			if (!IsSupportedCurrentAsset())
			{
				return LOCTEXT("StandaloneHistoryRestoreUnsupportedAsset", "Restore is available only for .uasset assets.");
			}

			const FRevisionPtr& Revision = SelectedRevisions[0];
			if (!HasRevisionBlob(Revision))
			{
				return LOCTEXT("StandaloneHistoryRestoreMissingBlob", "The selected revision has no recoverable blob; deleted revisions cannot be restored.");
			}
			if (!HasCurrentHistoricalPath(Revision))
			{
				return FText::Format(
					LOCTEXT("StandaloneHistoryRestoreHistoricalPathMismatch", "Restore is unavailable because the historical path '{0}' differs from the current path '{1}'. Case-only renames are treated as different paths."),
					FText::FromString(Revision->Filename), FText::FromString(Filename));
			}

			return LOCTEXT("StandaloneHistoryRestoreAvailable", "Force restore this revision to the workspace, reset its Git index entry to HEAD, and reload the affected package.");
		}
		EVisibility GetSingleRevisionActionVisibility() const { return CanDiffAgainstWorkspace() ? EVisibility::Visible : EVisibility::Collapsed; }
		EVisibility GetPreviousRevisionActionVisibility() const { return CanDiffAgainstPrevious() ? EVisibility::Visible : EVisibility::Collapsed; }
		EVisibility GetTwoRevisionActionVisibility() const { return CanDiffSelectedRevisions() ? EVisibility::Visible : EVisibility::Collapsed; }

		FReply OnDiffAgainstWorkspace()
		{
			ExecuteDiffAgainstWorkspace();
			return FReply::Handled();
		}

		FReply OnDiffAgainstPrevious()
		{
			ExecuteDiffAgainstPrevious();
			return FReply::Handled();
		}

		FReply OnDiffSelectedRevisions()
		{
			ExecuteDiffSelectedRevisions();
			return FReply::Handled();
		}

		FReply OnRestoreSelected()
		{
			ExecuteRestoreSelected();
			return FReply::Handled();
		}

		void ExecuteDiffAgainstWorkspace()
		{
			const TOptional<FHistoryDiffRequest> Request = MakeWorkspaceDiffRequest(GetSingleSelectedRevision());
			if (Request.IsSet() && OnDiff.IsBound())
			{
				TrackActiveDiff(OnDiff.Execute(Filename, Request->OlderRevision, nullptr));
			}
		}

		void ExecuteDiffAgainstPrevious()
		{
			const TOptional<FHistoryDiffRequest> Request = MakePreviousDiffRequest(Revisions, GetSingleSelectedRevision());
			if (Request.IsSet() && OnDiff.IsBound())
			{
				TrackActiveDiff(OnDiff.Execute(Filename, Request->OlderRevision, Request->NewerRevision));
			}
		}

		void ExecuteDiffSelectedRevisions()
		{
			const TOptional<FHistoryDiffRequest> Request = MakeSelectedRevisionDiffRequest(Revisions, GetSelectedRevisions());
			if (Request.IsSet() && OnDiff.IsBound())
			{
				TrackActiveDiff(OnDiff.Execute(Filename, Request->OlderRevision, Request->NewerRevision));
			}
		}

		void ExecuteRestoreSelected()
		{
			const FRevisionPtr Revision = GetSingleSelectedRevision();
			if (CanRestoreSelectedRevision())
			{
				OnRestore.ExecuteIfBound(Filename, Revision->CommitId, Revision->Filename);
			}
		}

		FText GetSelectedRevisionText() const { return LastSelectedRevision.IsValid() ? FText::FromString(LastSelectedRevision->CommitId) : FText::GetEmpty(); }
		FText GetSelectedDateText() const { return LastSelectedRevision.IsValid() ? GetDateText(*LastSelectedRevision) : FText::GetEmpty(); }
		FText GetSelectedAuthorText() const { return LastSelectedRevision.IsValid() ? FText::FromString(LastSelectedRevision->UserName) : FText::GetEmpty(); }
		FText GetSelectedActionText() const { return LastSelectedRevision.IsValid() ? FText::FromString(LastSelectedRevision->Action) : FText::GetEmpty(); }
		FText GetSelectedPathText() const { return LastSelectedRevision.IsValid() ? FText::FromString(LastSelectedRevision->Filename) : FText::GetEmpty(); }
		FText GetSelectedDescriptionText() const { return LastSelectedRevision.IsValid() ? FText::FromString(LastSelectedRevision->Description) : FText::GetEmpty(); }

		FReply OnRefreshHistory()
		{
			OnRefresh.ExecuteIfBound(Filename, Mode);
			return OnClose();
		}

		FReply OnClose()
		{
			CancelActiveDiffs();
			if (FSlateApplication::IsInitialized())
			{
				if (const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared()))
				{
					FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
				}
			}
			return FReply::Handled();
		}

		void TrackActiveDiff(const FGitStandaloneHistoryDiffCancellationPtr& CancellationContext)
		{
			for (int32 Index = ActiveDiffCancellationContexts.Num() - 1; Index >= 0; --Index)
			{
				if (!ActiveDiffCancellationContexts[Index].IsValid())
				{
					ActiveDiffCancellationContexts.RemoveAtSwap(Index);
				}
			}
			if (CancellationContext.IsValid())
			{
				ActiveDiffCancellationContexts.Add(CancellationContext);
			}
		}

		void CancelActiveDiffs()
		{
			for (const TWeakPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& Existing : ActiveDiffCancellationContexts)
			{
				if (const FGitStandaloneHistoryDiffCancellationPtr CancellationContext = Existing.Pin())
				{
					CancellationContext->Cancel();
				}
			}
			ActiveDiffCancellationContexts.Empty();
		}

		FString Filename;
		EGitLocalSourceControlHistoryMode Mode = EGitLocalSourceControlHistoryMode::CurrentPath;
		TArray<FRevisionPtr> Revisions;
		TArray<FHistoryTreeItemPtr> RootItems;
		TSharedPtr<STreeView<FHistoryTreeItemPtr>> HistoryTree;
		TSharedPtr<SHorizontalBox> RevisionActionBar;
		FRevisionPtr LastSelectedRevision;
		FGitStandaloneHistoryRestoreDelegate OnRestore;
		FGitStandaloneHistoryRefreshDelegate OnRefresh;
		FGitStandaloneHistoryDiffDelegate OnDiff;
		TArray<TWeakPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>> ActiveDiffCancellationContexts;
	};
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStandaloneHistoryDiffSelectionTest, "Cthulhu.GitSourceControl.Standalone.DiffSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitStandaloneHistoryDiffSelectionTest::RunTest(const FString& Parameters)
{
	using namespace SGitStandaloneHistoryWindowPrivate;
	const auto MakeRevision = [](const TCHAR* Commit, const TCHAR* Path)
	{
		const FRevisionPtr Revision = MakeShared<FGitSourceControlRevision, ESPMode::ThreadSafe>();
		Revision->CommitId = Commit;
		Revision->Filename = Path;
		return Revision;
	};

	const FRevisionPtr Newest = MakeRevision(TEXT("newest"), TEXT("Content/Newest.uasset"));
	const FRevisionPtr Previous = MakeRevision(TEXT("previous"), TEXT("Content/Previous.uasset"));
	const FRevisionPtr Oldest = MakeRevision(TEXT("oldest"), TEXT("Content/Oldest.uasset"));
	const FRevisionPtr Deleted = MakeRevision(TEXT("deleted"), TEXT("Content/Deleted.uasset"));
	Deleted->Action = TEXT("D");
	const TArray<FRevisionPtr> Snapshot = { Newest, Previous, Oldest };
	TestTrue(TEXT("Workspace Diff keeps the selected revision as older side"), MakeWorkspaceDiffRequest(Newest).IsSet());
	const TOptional<FHistoryDiffRequest> PreviousRequest = MakePreviousDiffRequest(Snapshot, Newest);
	TestTrue(TEXT("Previous Diff uses the snapshot-adjacent older revision"), PreviousRequest.IsSet() && PreviousRequest->OlderRevision == Previous && PreviousRequest->NewerRevision == Newest);
	TestFalse(TEXT("Oldest revision has no previous Diff target"), MakePreviousDiffRequest(Snapshot, Oldest).IsSet());
	const TOptional<FHistoryDiffRequest> SelectedRequest = MakeSelectedRevisionDiffRequest(Snapshot, { Oldest, Newest });
	TestTrue(TEXT("Selected revisions use snapshot order instead of click order"), SelectedRequest.IsSet() && SelectedRequest->OlderRevision == Oldest && SelectedRequest->NewerRevision == Newest);
	TestFalse(TEXT("A delete revision has no single-selection Diff action"), MakeWorkspaceDiffRequest(Deleted).IsSet());
	TestFalse(TEXT("A delete revision has no previous Diff action"), MakePreviousDiffRequest({ Newest, Deleted }, Deleted).IsSet());
	TestFalse(TEXT("A selection containing a delete revision has no two-revision Diff action"), MakeSelectedRevisionDiffRequest({ Newest, Deleted }, { Newest, Deleted }).IsSet());
	return true;
}
#endif

TSharedRef<SWindow> GitSourceControlStandaloneHistory::CreateWindow(const FString& Filename, const EGitLocalSourceControlHistoryMode Mode, const TGitSourceControlHistory& History, FGitStandaloneHistoryRestoreDelegate OnRestore, FGitStandaloneHistoryRefreshDelegate OnRefresh, FGitStandaloneHistoryDiffDelegate OnDiff)
{
	using namespace SGitStandaloneHistoryWindowPrivate;
	return SNew(SWindow)
		.Title(LOCTEXT("StandaloneHistoryWindowTitle", "File History"))
		.ClientSize(FVector2D(1000.0f, 400.0f))
		.SizingRule(ESizingRule::UserSized)
		.AutoCenter(EAutoCenter::PreferredWorkArea)
		.SupportsMinimize(true)
		.SupportsMaximize(true)
		[
			SNew(SGitStandaloneHistoryWindow)
			.Filename(Filename)
			.Mode(Mode)
			.History(History)
			.OnRestore(OnRestore)
			.OnRefresh(OnRefresh)
			.OnDiff(OnDiff)
		];
}

#undef LOCTEXT_NAMESPACE
