// Copyright Project Borealis

#pragma once

#include "CoreMinimal.h"

#include "HAL/Runnable.h"

#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"

/**
 *
 */
class FGitSourceControlRunner : public FRunnable
{
public:
	FGitSourceControlRunner();

	// Destructor
	virtual ~FGitSourceControlRunner() override;

	bool Init() override;
	uint32 Run() override;
	void Stop() override;
	void OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);


private:
	FRunnableThread* Thread;
	FEvent* StopEvent;
	bool bRunThread;
	bool bRefreshSpawned;
};
