// Copyright Project Borealis

#include "GitSourceControlRunner.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"

#include "Async/Async.h"

FGitSourceControlRunner::FGitSourceControlRunner()
{
	bRunThread = true;
	bRefreshSpawned = false;
	StopEvent = FPlatformProcess::GetSynchEventFromPool(true);
	Thread = FRunnableThread::Create(this, TEXT("GitSourceControlRunner"));
}

FGitSourceControlRunner::~FGitSourceControlRunner()
{
	if (Thread)
	{
		Thread->Kill();
		delete StopEvent;
		delete Thread;
	}
}

bool FGitSourceControlRunner::Init()
{
	return true;
}

uint32 FGitSourceControlRunner::Run()
{
	while (bRunThread)
	{
		StopEvent->Wait(30000);
		if (!bRunThread)
		{
			break;
		}
		// If we're not running the task already
		if (!bRefreshSpawned)
		{
			// Flag that we're running the task already
			bRefreshSpawned = true;
			const auto ExecuteResult = Async(EAsyncExecution::TaskGraphMainThread, [this] {
				FGitSourceControlModule* GitSourceControl = FGitSourceControlModule::GetThreadSafe();
				// Module not loaded, bail. Usually happens when editor is shutting down, and this prevents a crash from bad timing.
				if (!GitSourceControl)
				{
					return ECommandResult::Failed;
				}
				FGitSourceControlProvider& Provider = GitSourceControl->GetProvider();
				TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FGitFetch>();
				RefreshOperation->bUpdateStatus = true;
#if ENGINE_MAJOR_VERSION >= 5
				const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FSourceControlChangelistPtr(), FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlRunner::OnSourceControlOperationComplete));
#else
				const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlRunner::OnSourceControlOperationComplete));
#endif
				return Result;
				});
			// Wait for result if not already completed
			if (bRefreshSpawned && bRunThread)
			{
				// Get the result
				ECommandResult::Type Result = ExecuteResult.Get();
				// If still not completed,
				if (bRefreshSpawned)
				{
					// mark failures as done, successes have to complete
					bRefreshSpawned = Result == ECommandResult::Succeeded;
				}
			}
		}
	}

	return 0;
}

void FGitSourceControlRunner::Stop()
{
	bRunThread = false;
	StopEvent->Trigger();
}

void FGitSourceControlRunner::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	// Mark task as done
	bRefreshSpawned = false;
}
