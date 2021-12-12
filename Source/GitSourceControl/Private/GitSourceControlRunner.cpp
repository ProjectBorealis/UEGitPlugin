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
		if (!bRefreshSpawned)
		{
			bRefreshSpawned = true;
			const auto ExecuteResult = Async(EAsyncExecution::TaskGraphMainThread, [=] {
				FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
				FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
				TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FGitFetch>();
				RefreshOperation->bUpdateStatus = true;
				const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
					FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlRunner::OnSourceControlOperationComplete));
				return Result;
				});
			if (bRefreshSpawned && bRunThread)
			{
				ECommandResult::Type Result = ExecuteResult.Get();
				if (bRefreshSpawned)
				{
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
	bRefreshSpawned = false;
}
