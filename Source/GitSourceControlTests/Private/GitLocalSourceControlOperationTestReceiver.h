// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"
#include "GitLocalSourceControl.h"

#include "GitLocalSourceControlOperationTestReceiver.generated.h"

UCLASS(NotBlueprintType, meta = (NotInAngelscript))
class UGitLocalSourceControlOperationTestReceiver final : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleProgress(EGitLocalSourceControlOperationPhase InPhase)
	{
		ProgressPhases.Add(InPhase);
		if (InPhase == EGitLocalSourceControlOperationPhase::Completed
			|| InPhase == EGitLocalSourceControlOperationPhase::Cancelled
			|| InPhase == EGitLocalSourceControlOperationPhase::Failed)
		{
			++TerminalProgressCount;
		}
	}

	UFUNCTION()
	void HandleCompleted(FGitLocalSourceControlOperationResult InResult)
	{
		++CompletedCount;
		bCompletedAfterTerminalProgress = TerminalProgressCount == 1;
		CompletedResult = InResult;
	}

	TArray<EGitLocalSourceControlOperationPhase> ProgressPhases;
	FGitLocalSourceControlOperationResult CompletedResult;
	int32 TerminalProgressCount = 0;
	int32 CompletedCount = 0;
	bool bCompletedAfterTerminalProgress = false;
};
