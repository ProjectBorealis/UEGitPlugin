// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlChangelist.h"
#include "ISourceControlProvider.h"
#include "Misc/IQueuedWork.h"
#include "Runtime/Launch/Resources/Version.h"

/** Accumulated error and info messages for a revision control operation.  */
struct FGitSourceControlResultInfo
{
	/** Append any messages from another FSourceControlResultInfo, ensuring to keep any already accumulated info. */
	void Append(const FGitSourceControlResultInfo& InResultInfo)
	{
		InfoMessages.Append(InResultInfo.InfoMessages);
		ErrorMessages.Append(InResultInfo.ErrorMessages);
	}

	/** Info and/or warning message storage */
	TArray<FString> InfoMessages;

	/** Potential error message storage */
	TArray<FString> ErrorMessages;
};


/**
 * Used to execute Git commands multi-threaded.
 */
class FGitSourceControlCommand : public IQueuedWork
{
public:

	FGitSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete());

	/**
	 *  Modify the repo root if all selected files are in a plugin subfolder, and the plugin subfolder is a git repo
	 *  This supports the case where each plugin is a sub module
	 */
	void UpdateRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths);

	/**
	 * This is where the real thread work is done. All work that is done for
	 * this queued object should be done from within the call to this function.
	 */
	bool DoWork();

	/**
	 * Tells the queued work that it is being abandoned so that it can do
	 * per object clean up as needed. This will only be called if it is being
	 * abandoned before completion. NOTE: This requires the object to delete
	 * itself using whatever heap it was allocated in.
	 */
	virtual void Abandon() override;

	/**
	 * This method is also used to tell the object to cleanup but not before
	 * the object has finished it's work.
	 */
	virtual void DoThreadedWork() override;

	/** Attempt to cancel the operation */
	void Cancel();

	/** Is the operation canceled? */
	bool IsCanceled() const;

	/** Save any results and call any registered callbacks. */
	ECommandResult::Type ReturnResults();

public:
	/** Path to the Git binary */
	FString PathToGitBinary;

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	FString PathToRepositoryRoot;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToGitRoot;

	/** Tell if using the Git LFS file Locking workflow */
	bool bUsingGitLfsLocking;

	/** Operation we want to perform - contains outward-facing parameters & results */
	TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe> Operation;

	/** The object that will actually do the work */
	TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe> Worker;

	/** Delegate to notify when this operation completes */
	FSourceControlOperationComplete OperationCompleteDelegate;

	/**If true, this command has been processed by the revision control thread*/
	volatile int32 bExecuteProcessed;

	/**If true, this command has been cancelled*/
	volatile int32 bCancelled;

	/**If true, the revision control command succeeded*/
	bool bCommandSuccessful;

	/** Current Commit full SHA1 */
	FString CommitId;

	/** Current Commit description's Summary */
	FString CommitSummary;

	/** If true, this command will be automatically cleaned up in Tick() */
	bool bAutoDelete;

	/** Whether we are running multi-treaded or not*/
	EConcurrency::Type Concurrency;

	/** Files to perform this operation on */
	TArray<FString> Files;

#if ENGINE_MAJOR_VERSION == 5
    /** Changelist to perform this operation on */
    FGitSourceControlChangelist Changelist;
#endif

	/** Potential error, warning and info message storage */
	FGitSourceControlResultInfo ResultInfo;

	/** Branch names for status queries */
	TArray< FString > StatusBranchNames;
};
