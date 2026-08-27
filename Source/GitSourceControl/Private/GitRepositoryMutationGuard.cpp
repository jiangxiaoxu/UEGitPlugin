// Copyright (c) 2026

#include "GitRepositoryMutationGuard.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"

namespace GitSourceControlRepositoryMutationPrivate
{
	FCriticalSection RepositoryLocksMutex;
	TMap<FString, TSharedRef<FCriticalSection, ESPMode::ThreadSafe>> RepositoryLocks;

	FString NormalizeRepositoryKey(const FString& InRepositoryRoot)
	{
		FString Key = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
		FPaths::NormalizeDirectoryName(Key);
#if PLATFORM_WINDOWS
		Key.ToLowerInline();
#endif
		return Key;
	}
}

namespace GitSourceControlRepositoryMutation
{
	FGitRepositoryMutationGuard::FGitRepositoryMutationGuard(const FString& InRepositoryRoot)
		: Mutex(GetMutex(InRepositoryRoot))
	{
	}

	FGitRepositoryMutationGuard::~FGitRepositoryMutationGuard()
	{
		if (bLocked)
		{
			Mutex->Unlock();
		}
	}

	bool FGitRepositoryMutationGuard::Acquire(TFunctionRef<bool()> IsCancellationRequested)
	{
		while (!Mutex->TryLock())
		{
			if (IsCancellationRequested())
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.005f);
		}
		bLocked = true;
		if (IsCancellationRequested())
		{
			Mutex->Unlock();
			bLocked = false;
			return false;
		}
		return true;
	}

	bool FGitRepositoryMutationGuard::Acquire()
	{
		return Acquire([]() { return false; });
	}

	TSharedRef<FCriticalSection, ESPMode::ThreadSafe> FGitRepositoryMutationGuard::GetMutex(const FString& InRepositoryRoot)
	{
		const FString Key = GitSourceControlRepositoryMutationPrivate::NormalizeRepositoryKey(InRepositoryRoot);
		FScopeLock Lock(&GitSourceControlRepositoryMutationPrivate::RepositoryLocksMutex);
		if (const TSharedRef<FCriticalSection, ESPMode::ThreadSafe>* Existing = GitSourceControlRepositoryMutationPrivate::RepositoryLocks.Find(Key))
		{
			return *Existing;
		}
		const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> NewMutex = MakeShared<FCriticalSection, ESPMode::ThreadSafe>();
		GitSourceControlRepositoryMutationPrivate::RepositoryLocks.Add(Key, NewMutex);
		return NewMutex;
	}
}
