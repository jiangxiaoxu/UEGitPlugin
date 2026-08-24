// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"

#include "ISourceControlOperation.h"

/** Validates the local Git executable and repository. It never contacts a remote. */
class FGitConnectWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
};

/** Queries local state, and loads file history only when the caller explicitly requests it. */
class FGitUpdateStatusWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Immutable command results applied by the provider on the game thread. */
	TMap<const FString, FGitState> States;
	TMap<FString, TGitSourceControlHistory> Histories;
	/** Generation captured when a history query started; applied on the game thread. */
	uint64 HistoryGeneration = 0;
};
