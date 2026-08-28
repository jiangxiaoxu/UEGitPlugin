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
#include "GitStandaloneHistory.h"
#include "GitSourceControlUtils.h"
#include "SGitChangedAssetsPanel.h"
#include "Async/Async.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY(LogGitStandalone);

#define LOCTEXT_NAMESPACE "GitSourceControl"

class FGitSourceControlStartupProbeState final
{
public:
	TAtomic<bool> bShuttingDown = false;
};

namespace GitSourceControlModulePrivate
{
	const FName ChangedAssetsTabId(TEXT("GitChangedAssets"));
}

void FGitSourceControlModule::StartupModule()
{
	UE_LOG(LogGitStandalone, Display, TEXT("GitSourceControl standalone module starting."));
	GitLocalSourceControl::StartupOperations();
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
			.SetAutoGenerateMenuEntry(false)
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Edit"));
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGitSourceControlModule::RegisterStatusBarIntegration));
	}
	BeginStartupGitCapabilityProbe();
	PreExitHandle = FCoreDelegates::OnPreExit.AddRaw(this, &FGitSourceControlModule::HandlePreExit);
}

void FGitSourceControlModule::ShutdownModule()
{
	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}
	if (StartupProbeState.IsValid())
	{
		StartupProbeState->bShuttingDown.Store(true);
		if (StartupProbeCancellation.IsValid())
		{
			StartupProbeCancellation->Cancel();
		}
		if (StartupProbeCompletedEvent != nullptr)
		{
			StartupProbeCompletedEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(StartupProbeCompletedEvent);
			StartupProbeCompletedEvent = nullptr;
		}
		StartupProbeCancellation.Reset();
		StartupProbeState.Reset();
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
	GitSourceControlUtils::ClearStandaloneHistoryCache();
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
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

void FGitSourceControlModule::RegisterStatusBarIntegration()
{
	if (StatusBarIntegration.IsValid())
	{
		StatusBarIntegration->Install();
	}
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
	if (!GitSourceControlUtils::IsStartupGitCapabilityAvailable())
	{
		ShowStartupGitCapabilityDialog();
		return;
	}
	FGlobalTabmanager::Get()->TryInvokeTab(GitSourceControlModulePrivate::ChangedAssetsTabId);
}

void FGitSourceControlModule::BeginStartupGitCapabilityProbe()
{
	check(IsInGameThread());
	if (!GitSourceControlUtils::BeginStartupGitCapabilityProbe())
	{
		return;
	}
	StartupProbeState = MakeShared<FGitSourceControlStartupProbeState, ESPMode::ThreadSafe>();
	StartupProbeCancellation = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
	StartupProbeCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
	StartupProbeCompletedEvent->Reset();
	const TWeakPtr<FGitSourceControlStartupProbeState, ESPMode::ThreadSafe> WeakProbeState = StartupProbeState;
	const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> Cancellation = StartupProbeCancellation.ToSharedRef();
	FEvent* const CompletionEvent = StartupProbeCompletedEvent;
	Async(EAsyncExecution::ThreadPool, [WeakProbeState, Cancellation, CompletionEvent, this]()
	{
		GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(Cancellation);
		GitSourceControlUtils::FGitStartupCapability Capability = GitSourceControlUtils::ProbeStartupGitCapability();
		if (Cancellation->IsCancellationRequested())
		{
			Capability.State = GitSourceControlUtils::EGitStartupCapabilityState::Unavailable;
			Capability.GitBinary.Reset();
			Capability.Diagnostic = TEXT("Git startup capability detection was cancelled during Editor shutdown.");
		}
		GitSourceControlUtils::CompleteStartupGitCapabilityProbe(MoveTemp(Capability));
#if WITH_DEV_AUTOMATION_TESTS
		GitSourceControlUtils::Testing::CaptureGitProcessLaunchCountAtModuleStartup();
#endif
		AsyncTask(ENamedThreads::GameThread, [WeakProbeState, this]()
		{
			if (const TSharedPtr<FGitSourceControlStartupProbeState, ESPMode::ThreadSafe> ProbeState = WeakProbeState.Pin())
			{
				if (!ProbeState->bShuttingDown.Load())
				{
					HandleStartupGitCapabilityCompleted();
				}
			}
		});
		CompletionEvent->Trigger();
	});
}

void FGitSourceControlModule::HandleStartupGitCapabilityCompleted()
{
	check(IsInGameThread());
	if (ChangedAssetsController.IsValid())
	{
		ChangedAssetsController->HandleStartupGitCapabilityChanged();
	}
}

void FGitSourceControlModule::ShowStartupGitCapabilityDialog() const
{
	FMessageDialog::Open(EAppMsgType::Ok, GitSourceControlUtils::GetStartupGitCapabilityMessage(),
		LOCTEXT("GitCapabilityUnavailableTitle", "Git Changes Unavailable"));
}

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);

#undef LOCTEXT_NAMESPACE
