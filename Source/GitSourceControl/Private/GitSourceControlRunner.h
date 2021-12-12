// Copyright Project Borealis

#pragma once

#include "CoreMinimal.h"

#include "HAL/Runnable.h"

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
	bool bRunThread;
	bool bRefreshSpawned;
};
