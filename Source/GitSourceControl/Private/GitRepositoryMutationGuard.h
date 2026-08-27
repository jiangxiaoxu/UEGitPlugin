// Copyright (c) 2026
//
// Shared per-repository transaction serialization for providerless Git mutations.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

namespace GitSourceControlRepositoryMutation
{
	class GITSOURCECONTROL_API FGitRepositoryMutationGuard final
	{
	public:
		explicit FGitRepositoryMutationGuard(const FString& InRepositoryRoot);
		~FGitRepositoryMutationGuard();

		bool Acquire(TFunctionRef<bool()> IsCancellationRequested);
		bool Acquire();

		FGitRepositoryMutationGuard(const FGitRepositoryMutationGuard&) = delete;
		FGitRepositoryMutationGuard& operator=(const FGitRepositoryMutationGuard&) = delete;

	private:
		static TSharedRef<FCriticalSection, ESPMode::ThreadSafe> GetMutex(const FString& InRepositoryRoot);

		TSharedRef<FCriticalSection, ESPMode::ThreadSafe> Mutex;
		bool bLocked = false;
	};
}
