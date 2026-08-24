// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "SGitSourceControlSettings.h"

#include "GitSourceControlModule.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SGitSourceControlSettings"

void SGitSourceControlSettings::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("GitBinaryPathLabel", "Git Executable"))
				.ToolTipText(LOCTEXT("GitBinaryPathTooltip", "Automatically resolved local Git executable"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SGitSourceControlSettings::GetGitBinaryPath)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RepositoryRootLabel", "Local Repository"))
				.ToolTipText(LOCTEXT("RepositoryRootTooltip", "Git repository containing this project"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SGitSourceControlSettings::GetPathToRepositoryRoot)
			]
		]
	];
}

FText SGitSourceControlSettings::GetGitBinaryPath() const
{
	return FText::FromString(FGitSourceControlModule::Get().GetProvider().GetGitBinaryPath());
}

FText SGitSourceControlSettings::GetPathToRepositoryRoot() const
{
	return FText::FromString(FGitSourceControlModule::Get().GetProvider().GetPathToRepositoryRoot());
}

#undef LOCTEXT_NAMESPACE
