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
	Thread = FRunnableThread::Create(this, TEXT("GitSourceControlRunner"));
}

FGitSourceControlRunner::~FGitSourceControlRunner()
{
	if (Thread)
	{
		// Kill() is a blocking call, it waits for the thread to finish.
		// Hopefully that doesn't take too long
		Thread->Kill();
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
		FPlatformProcess::Sleep(30.0f);
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
			ECommandResult::Type Result = ExecuteResult.Get();
			if (bRefreshSpawned)
			{
				bRefreshSpawned = Result == ECommandResult::Succeeded;
			}
		}
	}

	return 0;
}

void FGitSourceControlRunner::Stop()
{
	bRunThread = false;
}

void FGitSourceControlRunner::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	bRefreshSpawned = false;
}
