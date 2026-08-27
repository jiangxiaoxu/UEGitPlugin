// Copyright (c) 2026

#include "GitSourceControlStatusBarIntegration.h"

#include "ToolMenu.h"
#include "ToolMenuContext.h"
#include "ToolMenuEntry.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "UnsavedAssetsTrackerModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlStatusBarIntegrationPrivate
{
	const FName StatusBarMenuName(TEXT("LevelEditor.StatusBar.ToolBar"));
	const FName SourceControlSectionName(TEXT("SourceControl"));
	const FName SourceControlEntryName(TEXT("SourceControl"));

	TSharedRef<SWidget> MakeChangedAssetsWidget(const FSimpleDelegate& OpenChangedAssets)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				FUnsavedAssetsTrackerModule::Get().MakeUnsavedAssetsStatusBarWidget()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, -5.0f)
			[
				SNew(SSeparator)
				.Thickness(2.0f)
				.Orientation(EOrientation::Orient_Vertical)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(6.0f, 0.0f))
				.ToolTipText(LOCTEXT("ChangedAssetsStatusBarTooltip", "Open Git Changes"))
				.OnClicked_Lambda([OpenChangedAssets]()
				{
					OpenChangedAssets.ExecuteIfBound();
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(FAppStyle::Get().GetBrush("SourceControl.Edit"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(5.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
						.Text(LOCTEXT("ChangedAssetsStatusBarLabel", "Git Changes"))
					]
				]
			];
	}

	bool IsExpectedOriginalEntry(const FToolMenuEntry& Entry)
	{
		// SStatusBar::RegisterSourceControlStatus 当前创建的是无 Owner widget，且布局标志固定。
		// 其他形态一律拒绝，避免静默覆盖其他插件的 SourceControl entry。
		return Entry.Name == SourceControlEntryName
			&& Entry.Type == EMultiBlockType::Widget
			&& !Entry.Owner.IsSet()
			&& Entry.WidgetData.bNoIndent
			&& !Entry.WidgetData.bSearchable
			&& !Entry.WidgetData.bNoPadding;
	}
}

FGitSourceControlStatusBarIntegration::FGitSourceControlStatusBarIntegration(FSimpleDelegate InOpenChangedAssets)
	: OpenChangedAssets(MoveTemp(InOpenChangedAssets))
{
}

FGitSourceControlStatusBarIntegration::~FGitSourceControlStatusBarIntegration()
{
	Uninstall();
}

void FGitSourceControlStatusBarIntegration::Install()
{
	UToolMenus* ToolMenus = UToolMenus::TryGet();
	if (!ToolMenus)
	{
		return;
	}

	if (!PreGenerateHandle.IsValid())
	{
		PreGenerateHandle = ToolMenus->OnPreGenerateWidget.AddRaw(this, &FGitSourceControlStatusBarIntegration::HandlePreGenerateWidget);
	}

	if (TryReplaceEntry())
	{
		ToolMenus->RefreshAllWidgets();
	}
}

void FGitSourceControlStatusBarIntegration::Uninstall()
{
	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		if (PreGenerateHandle.IsValid())
		{
			ToolMenus->OnPreGenerateWidget.Remove(PreGenerateHandle);
			PreGenerateHandle.Reset();
		}
		const bool bHadOriginalEntry = OriginalEntry.IsSet();
		RestoreOriginalEntry();
		if (bHadOriginalEntry)
		{
			// 目标 toolbar 必须同步重建，确保旧 widget 在 DLL 卸载前释放。
			ToolMenus->RefreshMenuWidget(GitSourceControlStatusBarIntegrationPrivate::StatusBarMenuName);
		}
		ToolMenus->RefreshAllWidgets();
	}
	else
	{
		PreGenerateHandle.Reset();
	}
}

void FGitSourceControlStatusBarIntegration::HandlePreGenerateWidget(const FName InMenuName, const FToolMenuContext& InMenuContext)
{
	(void)InMenuContext;
	if (InMenuName == GitSourceControlStatusBarIntegrationPrivate::StatusBarMenuName)
	{
		TryReplaceEntry();
	}
}

bool FGitSourceControlStatusBarIntegration::TryReplaceEntry()
{
	UToolMenus* ToolMenus = UToolMenus::TryGet();
	if (!ToolMenus || !ToolMenus->IsMenuRegistered(GitSourceControlStatusBarIntegrationPrivate::StatusBarMenuName))
	{
		return false;
	}

	UToolMenu* Toolbar = ToolMenus->FindMenu(GitSourceControlStatusBarIntegrationPrivate::StatusBarMenuName);
	FToolMenuSection* Section = Toolbar ? Toolbar->FindSection(GitSourceControlStatusBarIntegrationPrivate::SourceControlSectionName) : nullptr;
	if (!Section)
	{
		return false;
	}

	int32 SourceControlIndex = INDEX_NONE;
	int32 SourceControlCount = 0;
	for (int32 Index = 0; Index < Section->Blocks.Num(); ++Index)
	{
		if (Section->Blocks[Index].Name == GitSourceControlStatusBarIntegrationPrivate::SourceControlEntryName)
		{
			SourceControlIndex = Index;
			++SourceControlCount;
		}
	}
	if (SourceControlCount != 1)
	{
		return false;
	}

	FToolMenuEntry& ExistingEntry = Section->Blocks[SourceControlIndex];
	if (ExistingEntry.Owner == FToolMenuOwner(this))
	{
		return false;
	}
	if (!GitSourceControlStatusBarIntegrationPrivate::IsExpectedOriginalEntry(ExistingEntry))
	{
		// 其他 Owner 或形态已经替换了 entry，保持原状并 fail-closed。
		return false;
	}

	if (!OriginalEntry.IsSet())
	{
		OriginalEntry = ExistingEntry;
		OriginalSectionName = GitSourceControlStatusBarIntegrationPrivate::SourceControlSectionName;
		OriginalBlockIndex = SourceControlIndex;
	}
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		FToolMenuEntry Replacement = FToolMenuEntry::InitWidget(
			GitSourceControlStatusBarIntegrationPrivate::SourceControlEntryName,
			GitSourceControlStatusBarIntegrationPrivate::MakeChangedAssetsWidget(OpenChangedAssets),
			FText::GetEmpty(),
			true,
			false,
			false,
			LOCTEXT("ChangedAssetsStatusBarEntryTooltip", "Open Git Changes"));
		Section->Blocks[SourceControlIndex] = MoveTemp(Replacement);
	}
	return true;
}

void FGitSourceControlStatusBarIntegration::RestoreOriginalEntry()
{
	if (!OriginalEntry.IsSet())
	{
		return;
	}

	UToolMenus* ToolMenus = UToolMenus::TryGet();
	UToolMenu* Toolbar = ToolMenus ? ToolMenus->FindMenu(GitSourceControlStatusBarIntegrationPrivate::StatusBarMenuName) : nullptr;
	FToolMenuSection* Section = Toolbar ? Toolbar->FindSection(OriginalSectionName) : nullptr;
	if (!Section)
	{
		return;
	}

	int32 UnknownEntryCount = 0;
	for (int32 Index = Section->Blocks.Num() - 1; Index >= 0; --Index)
	{
		if (Section->Blocks[Index].Name == GitSourceControlStatusBarIntegrationPrivate::SourceControlEntryName)
		{
			if (Section->Blocks[Index].Owner == FToolMenuOwner(this))
			{
				Section->Blocks.RemoveAt(Index);
			}
			else
			{
				++UnknownEntryCount;
			}
		}
	}
	if (UnknownEntryCount == 0)
	{
		Section->Blocks.Insert(OriginalEntry.GetValue(), FMath::Clamp(OriginalBlockIndex, 0, Section->Blocks.Num()));
	}
	// 未知 entry 保持原状；仅移除本插件创建的 entry。
	OriginalEntry.Reset();
	OriginalSectionName = NAME_None;
	OriginalBlockIndex = INDEX_NONE;
}

#undef LOCTEXT_NAMESPACE
