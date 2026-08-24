// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "Features/IModularFeatures.h"
#include "GitLocalSourceControl.h"
#include "GitSourceControlOperations.h"
#include "Misc/App.h"

TArray<FString> FGitSourceControlModule::EmptyStringArray;

namespace GitSourceControlModulePrivate
{
	const FName SourceControlFeatureName(TEXT("SourceControl"));

	template<typename WorkerType>
	TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker()
	{
		return MakeShared<WorkerType, ESPMode::ThreadSafe>();
	}
}

void FGitSourceControlModule::StartupModule()
{
	UE_LOG(LogSourceControl, Display, TEXT("GitSourceControl Phase 1 module starting."));
	// Phase 1 intentionally exposes only local connection and local status/history.
	// Asset mutations are explicit menu actions, not generic Source Control workers.
	GitSourceControlProvider.RegisterWorker(TEXT("Connect"), FGetGitSourceControlWorker::CreateStatic(&GitSourceControlModulePrivate::CreateWorker<FGitConnectWorker>));
	GitSourceControlProvider.RegisterWorker(TEXT("UpdateStatus"), FGetGitSourceControlWorker::CreateStatic(&GitSourceControlModulePrivate::CreateWorker<FGitUpdateStatusWorker>));

	IModularFeatures::Get().RegisterModularFeature(GitSourceControlModulePrivate::SourceControlFeatureName, &GitSourceControlProvider);
#if WITH_DEV_AUTOMATION_TESTS
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		if (FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("GitSourceControlTests")) == nullptr)
		{
			UE_LOG(LogSourceControl, Error, TEXT("GitSourceControlTests could not be loaded for unattended automation."));
		}
		else
		{
			UE_LOG(LogSourceControl, Display, TEXT("GitSourceControlTests loaded for unattended automation."));
		}
	}
#endif
	if (!FApp::IsUnattended() && !IsRunningCommandlet())
	{
		GitSourceControlMenu.Register();
	}
}

void FGitSourceControlModule::ShutdownModule()
{
	GitLocalSourceControl::ShutdownOperations();
	GitSourceControlMenu.Unregister();
	GitSourceControlProvider.Close();
	IModularFeatures::Get().UnregisterModularFeature(GitSourceControlModulePrivate::SourceControlFeatureName, &GitSourceControlProvider);
}

void FGitSourceControlModule::SetLastErrors(const TArray<FText>& InErrors)
{
	if (FGitSourceControlModule* Module = FModuleManager::GetModulePtr<FGitSourceControlModule>(TEXT("GitSourceControl")))
	{
		Module->GetProvider().SetLastErrors(InErrors);
	}
}

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);
