// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#include "GitSourceControlMenu.h"
#include "GitSourceControlProvider.h"

class FExtender;

/**
 * Local Git revision control integration for Unreal Editor.
 *
 * Phase 1 provides local status, History, Diff, exact asset discard, untracked
 * file deletion, and historical revision restore. An external Git GUI owns all
 * remote operations, staging, commits, branch changes, and conflict resolution.
 */
class FGitSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Access the Git revision control provider */
	FGitSourceControlProvider& GetProvider()
	{
		return GitSourceControlProvider;
	}

	const FGitSourceControlProvider& GetProvider() const
	{
		return GitSourceControlProvider;
	}

	GITSOURCECONTROL_API static const TArray< FString > & GetEmptyStringArray()
	{
		return EmptyStringArray;
	}

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

	static inline FGitSourceControlModule* GetThreadSafe()
	{
		IModuleInterface* ModulePtr = FModuleManager::Get().GetModule("GitSourceControl");
		if (!ModulePtr)
		{
			// Main thread should never have this unloaded.
			check(!IsInGameThread());
			return nullptr;
		}
		return static_cast<FGitSourceControlModule*>(ModulePtr);
	}

	/** Set list of error messages that occurred after last git command */
	static void SetLastErrors(const TArray<FText>& InErrors);

private:
	/** The one and only Git revision control provider */
	FGitSourceControlProvider GitSourceControlProvider;

	/** Editor-only local Git actions. */
	FGitSourceControlMenu GitSourceControlMenu;

	static TArray<FString> EmptyStringArray;

};
