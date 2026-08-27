// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Delegates/Delegate.h"

#include "GitSourceControlMenu.h"

class FExtender;
class FGitChangedAssetsController;
class FGitSourceControlStatusBarIntegration;
class SDockTab;
class FEvent;
class FGitSourceControlStartupProbeState;
namespace GitSourceControlUtils
{
	class FGitOperationCancellationContext;
}

/**
 * Standalone local Git asset tools for Unreal Editor.
 *
 * The module deliberately does not implement or register an Unreal Source Control
 * provider. Git discovery and commands begin only after an explicit plugin action.
 */
class FGitSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	/** ToolMenus/Slate delegate 与异步事务需要进程级生命周期。 */
	virtual bool SupportsDynamicReloading() override { return false; }

	/**
	 * Singleton-like access to this module's interface.  This is just for convenience!
	 * Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	 *
	 * @return Returns singleton instance, loading the module on demand if needed
	 */
	static inline FGitSourceControlModule& Get()
	{
		return FModuleManager::Get().LoadModuleChecked< FGitSourceControlModule >("GitSourceControl");
	}

private:
	/** Editor-only local Git actions. */
	FGitSourceControlMenu GitSourceControlMenu;
	TSharedPtr<FGitChangedAssetsController, ESPMode::ThreadSafe> ChangedAssetsController;
	TSharedPtr<FGitSourceControlStatusBarIntegration, ESPMode::ThreadSafe> StatusBarIntegration;

	void RegisterStatusBarIntegration();
	TSharedRef<SDockTab> SpawnChangedAssetsTab(const class FSpawnTabArgs& SpawnTabArgs);
	void OpenChangedAssetsTab();
	void BeginStartupGitCapabilityProbe();
	void HandleStartupGitCapabilityCompleted();
	void ShowStartupGitCapabilityDialog() const;
	void HandlePreExit();
	FDelegateHandle PreExitHandle;
	TSharedPtr<FGitSourceControlStartupProbeState, ESPMode::ThreadSafe> StartupProbeState;
	TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> StartupProbeCancellation;
	FEvent* StartupProbeCompletedEvent = nullptr;
};
