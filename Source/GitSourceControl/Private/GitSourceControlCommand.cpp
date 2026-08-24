// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlCommand.h"

#include "Modules/ModuleManager.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "HAL/PlatformProcess.h"

FGitSourceControlCommand::FGitSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
	, bExecuteProcessed(0)
	, bCancelled(0)
	, bCommandSuccessful(false)
	, CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
	, bResultsReturned(0)
	, bAutoDelete(true)
	, Concurrency(EConcurrency::Synchronous)
{
	// cache the providers settings here
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	PathToGitBinary = Provider.GetGitBinaryPath();
	PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	PathToGitRoot = Provider.GetPathToGitRoot();
}

FGitSourceControlCommand::~FGitSourceControlCommand()
{
	if (CompletionEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
		CompletionEvent = nullptr;
	}
}

void FGitSourceControlCommand::UpdateRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths)
{
	PathToRepositoryRoot = GitSourceControlUtils::ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);
}

bool FGitSourceControlCommand::DoWork()
{
	if (IsCanceled())
	{
		FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
		return false;
	}

	GitSourceControlUtils::SetActiveCommand(this);
	bCommandSuccessful = Worker->Execute(*this);
	GitSourceControlUtils::ClearActiveCommand(this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);

	return bCommandSuccessful;
}

void FGitSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
	CompletionEvent->Trigger();
}

void FGitSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
	// The thread pool may still access this IQueuedWork until this method returns.
	CompletionEvent->Trigger();
}

void FGitSourceControlCommand::Cancel()
{
	FPlatformAtomics::InterlockedExchange(&bCancelled, 1);
}

bool FGitSourceControlCommand::IsCanceled() const
{
	return bCancelled != 0;
}

bool FGitSourceControlCommand::WaitForCompletion(uint32 InTimeoutMilliseconds) const
{
	return CompletionEvent != nullptr && CompletionEvent->Wait(InTimeoutMilliseconds);
}

ECommandResult::Type FGitSourceControlCommand::ReturnResults()
{
	if (FPlatformAtomics::InterlockedCompareExchange(&bResultsReturned, 1, 0) != 0)
	{
		return IsCanceled() ? ECommandResult::Cancelled : (bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed);
	}

	// Save any messages that have accumulated
	for (const auto& String : ResultInfo.InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(String));
	}
	for (const auto& String : ResultInfo.ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(String));
	}

	// run the completion delegate if we have one bound
	ECommandResult::Type Result = bCancelled ? ECommandResult::Cancelled : (bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed);
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);

	return Result;
}
