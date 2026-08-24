// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "GitSourceControlCommand.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlUtils.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlOperationsPrivate
{
	bool AreEquivalentHistories(const TGitSourceControlHistory& Left, const TGitSourceControlHistory& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			const FGitSourceControlRevision& A = Left[Index].Get();
			const FGitSourceControlRevision& B = Right[Index].Get();
			if (A.CommitId != B.CommitId || A.Filename != B.Filename || A.LocalFilename != B.LocalFilename || A.Action != B.Action ||
				A.Description != B.Description || A.UserName != B.UserName || A.Date != B.Date || A.FileHash != B.FileHash || A.FileSize != B.FileSize)
			{
				return false;
			}
		}
		return true;
	}
}

FName FGitConnectWorker::GetName() const
{
	return TEXT("Connect");
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText Error = LOCTEXT("GitNotFound", "Failed to enable Git revision control. Configure a valid local Git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(Error.ToString());
		Operation->SetErrorText(Error);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	const bool bInsideWorkTree = GitSourceControlUtils::RunCommand(
		TEXT("rev-parse"),
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		{ TEXT("--is-inside-work-tree") },
		FGitSourceControlModule::GetEmptyStringArray(),
		Results,
		Errors);

	InCommand.ResultInfo.InfoMessages.Append(Results);
	InCommand.ResultInfo.ErrorMessages.Append(Errors);
	InCommand.bCommandSuccessful = bInsideWorkTree && Results.ContainsByPredicate([](const FString& Result)
	{
		return Result.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	});

	if (!InCommand.bCommandSuccessful)
	{
		const FText Error = LOCTEXT("GitRepositoryNotFound", "Failed to enable Git revision control. The project is not inside a local Git working tree.");
		InCommand.ResultInfo.ErrorMessages.Add(Error.ToString());
		Operation->SetErrorText(Error);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return false;
}

FName FGitUpdateStatusWorker::GetName() const
{
	return TEXT("UpdateStatus");
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());
	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	// An empty request must never turn into a repository-wide scan. The provider schedules
	// cached-state refreshes explicitly, with their exact paths.
	if (InCommand.Files.IsEmpty())
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}
	if (Operation->ShouldUpdateHistory())
	{
		// History is an explicit, single-file Git log request. Do not precede it
		// with status: opening History must not pay for an unrelated status pass.
		HistoryGeneration = GitSourceControlUtils::GetRepositoryGeneration(InCommand.PathToRepositoryRoot);
		for (const FString& Filename : InCommand.Files)
		{
			TGitSourceControlHistory History;
			if (!GitSourceControlUtils::RunGetHistory(
				InCommand.PathToGitBinary,
				InCommand.PathToRepositoryRoot,
				Filename,
				false,
				InCommand.ResultInfo.ErrorMessages,
				History))
			{
				InCommand.bCommandSuccessful = false;
				return false;
			}
			Histories.Add(Filename, MoveTemp(History));
		}
		InCommand.bCommandSuccessful = true;
		return true;
	}

	TMap<FString, FGitSourceControlState> UpdatedStates;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		false,
		InCommand.Files,
		InCommand.ResultInfo.ErrorMessages,
		UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	if (!InCommand.bCommandSuccessful)
	{
		return false;
	}

	GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	return true;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);
	FGitSourceControlModule* Module = FGitSourceControlModule::GetThreadSafe();
	if (!Module)
	{
		return bUpdated;
	}

	FGitSourceControlProvider& Provider = Module->GetProvider();
	for (const TPair<FString, TGitSourceControlHistory>& Pair : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Pair.Key);
		const FString LocalFilename = State->LocalFilename;
		for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : Pair.Value)
		{
			Revision->LocalFilename = LocalFilename;
			// Git rename history is linear. Do not expose a fake Perforce branch
			// source that causes the History widget to navigate an old workspace path.
			Revision->BranchSource.Reset();
		}
		if (!GitSourceControlOperationsPrivate::AreEquivalentHistories(State->History, Pair.Value))
		{
			State->History = Pair.Value;
			State->TimeStamp = FDateTime::Now();
			bUpdated = true;
		}
		if (State->HistoryGeneration != HistoryGeneration)
		{
			State->HistoryGeneration = HistoryGeneration;
			bUpdated = true;
		}
	}

	return bUpdated;
}

#undef LOCTEXT_NAMESPACE
