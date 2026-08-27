// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "GitChangedAssetsController.h"
#include "GitLocalSourceControl.h"
#include "GitSourceControlRevision.h"
#include "GitSourceControlStatusBarIntegration.h"
#include "GitStandaloneLog.h"
#include "GitSourceControlUtils.h"
#include "SGitChangedAssetsPanel.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogGitStandalone);

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlModulePrivate
{
	const FName ChangedAssetsTabId(TEXT("GitChangedAssets"));
}

void FGitSourceControlModule::StartupModule()
{
	UE_LOG(LogGitStandalone, Display, TEXT("GitSourceControl standalone module starting."));
#if WITH_DEV_AUTOMATION_TESTS
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		if (FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("GitSourceControlTests")) == nullptr)
		{
			UE_LOG(LogGitStandalone, Error, TEXT("GitSourceControlTests could not be loaded for unattended automation."));
		}
		else
		{
			UE_LOG(LogGitStandalone, Display, TEXT("GitSourceControlTests loaded for unattended automation."));
		}
	}
#endif
	if (!FApp::IsUnattended() && !IsRunningCommandlet())
	{
		GitSourceControlMenu.Register();
		ChangedAssetsController = MakeShared<FGitChangedAssetsController, ESPMode::ThreadSafe>();
		StatusBarIntegration = MakeShared<FGitSourceControlStatusBarIntegration, ESPMode::ThreadSafe>(
			FSimpleDelegate::CreateRaw(this, &FGitSourceControlModule::OpenChangedAssetsTab));
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GitSourceControlModulePrivate::ChangedAssetsTabId,
			FOnSpawnTab::CreateRaw(this, &FGitSourceControlModule::SpawnChangedAssetsTab))
			.SetDisplayName(LOCTEXT("ChangedAssetsTabName", "Git Changes"))
			.SetTooltipText(LOCTEXT("ChangedAssetsTabTooltip", "Shows repository-wide changed .uasset files and reverts selected assets to HEAD."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Edit"));
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGitSourceControlModule::RegisterMenus));
	}
	PreExitHandle = FCoreDelegates::OnPreExit.AddRaw(this, &FGitSourceControlModule::HandlePreExit);
#if WITH_DEV_AUTOMATION_TESTS
	GitSourceControlUtils::Testing::CaptureGitProcessLaunchCountAtModuleStartup();
#endif
}

void FGitSourceControlModule::ShutdownModule()
{
	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}
	if (ChangedAssetsController.IsValid())
	{
		ChangedAssetsController->Shutdown();
		ChangedAssetsController.Reset();
	}
	if (StatusBarIntegration.IsValid())
	{
		StatusBarIntegration->Uninstall();
		StatusBarIntegration.Reset();
	}
	GitLocalSourceControl::ShutdownOperations();
	GitSourceControlMenu.Unregister();
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
	if (FGlobalTabmanager::Get()->HasTabSpawner(GitSourceControlModulePrivate::ChangedAssetsTabId))
	{
		if (const TSharedPtr<SDockTab> ExistingTab = FGlobalTabmanager::Get()->FindExistingLiveTab(GitSourceControlModulePrivate::ChangedAssetsTabId))
		{
			ExistingTab->RequestCloseTab();
		}
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GitSourceControlModulePrivate::ChangedAssetsTabId);
	}
}

void FGitSourceControlModule::HandlePreExit()
{
	GitSourceControlRevision::CleanupTemporaryExports();
}

void FGitSourceControlModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	if (StatusBarIntegration.IsValid())
	{
		StatusBarIntegration->Install();
	}
	UToolMenu* const WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
	FToolMenuSection& WindowSection = WindowMenu->FindOrAddSection(TEXT("WindowLayout"));
	WindowSection.AddMenuEntry(
		TEXT("GitSourceControl_OpenChangedAssets"),
		LOCTEXT("OpenChangedAssets", "Git Changes"),
		LOCTEXT("OpenChangedAssetsTooltip", "Open the repository-wide Git Changes view."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Edit"),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlModule::OpenChangedAssetsTab)));
}

TSharedRef<SDockTab> FGitSourceControlModule::SpawnChangedAssetsTab(const FSpawnTabArgs& SpawnTabArgs)
{
	(void)SpawnTabArgs;
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SGitChangedAssetsPanel)
			.Controller(ChangedAssetsController)
		];
}

void FGitSourceControlModule::OpenChangedAssetsTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(GitSourceControlModulePrivate::ChangedAssetsTabId);
}

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);

#undef LOCTEXT_NAMESPACE
