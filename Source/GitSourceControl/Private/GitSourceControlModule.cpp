// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "GitLocalSourceControl.h"
#include "GitSourceControlRevision.h"
#include "GitStandaloneLog.h"
#include "GitSourceControlUtils.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogGitStandalone);

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
	GitLocalSourceControl::ShutdownOperations();
	GitSourceControlMenu.Unregister();
}

void FGitSourceControlModule::HandlePreExit()
{
	GitSourceControlRevision::CleanupTemporaryExports();
}

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);
