// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"

#include "ISourceControlOperation.h"

/**
 * Internal operation used to fetch from remote
 */
class FGitFetch : public ISourceControlOperation
{
public:
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;

	bool bUpdateStatus = false;
};

/** Called when first activated on a project, and then at project load time.
 *  Look for the root directory of the git repository (where the ".git/" subdirectory is located). */
class FGitConnectWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitConnectWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Lock (check-out) a set of files using Git LFS 2. */
class FGitCheckOutWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCheckOutWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Commit (check-in) a set of files to the local depot. */
class FGitCheckInWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCheckInWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Add an untracked file to revision control (so only a subset of the git add command). */
class FGitMarkForAddWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitMarkForAddWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Delete a file and remove it from revision control. */
class FGitDeleteWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitDeleteWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Revert any change to a file to its state on the local depot. */
class FGitRevertWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitRevertWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Git pull --rebase to update branch from its configured remote */
class FGitSyncWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitSyncWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Get revision control status of files on local working copy. */
class FGitUpdateStatusWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStatusWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	/** Temporary states for results */
	TMap<const FString, FGitState> States;

	/** Map of filenames to history */
	TMap<FString, TGitSourceControlHistory> Histories;
};

/** Copy or Move operation on a single file */
class FGitCopyWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCopyWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** git add to mark a conflict as resolved */
class FGitResolveWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitResolveWorker() {}
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Git push to publish branch for its configured remote */
class FGitFetchWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitFetchWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};
