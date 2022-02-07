// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"
#include "ISourceControlState.h"
#include "ISourceControlProvider.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"
#include "GitSourceControlMenu.h"
#include "Runtime/Launch/Resources/Version.h"

class FGitSourceControlCommand;

DECLARE_DELEGATE_RetVal(FGitSourceControlWorkerRef, FGetGitSourceControlWorker)

/// Git version and capabilites extracted from the string "git version 2.11.0.windows.3"
struct FGitVersion
{
	// Git version extracted from the string "git version 2.11.0.windows.3" (Windows), "git version 2.11.0" (Linux/Mac/Cygwin/WSL) or "git version 2.31.1.vfs.0.3" (Microsoft)
	int Major;   // 2	Major version number
	int Minor;   // 31	Minor version number
	int Patch;   // 1	Patch/bugfix number
	bool bIsFork;
	FString Fork; // "vfs"
	int ForkMajor; // 0	Fork specific revision number
	int ForkMinor; // 3 
	int ForkPatch; // ?

	uint32 bHasCatFileWithFilters : 1;
	uint32 bHasGitLfs : 1;
	uint32 bHasGitLfsLocking : 1;

	FGitVersion() 
		: Major(0)
		, Minor(0)
		, Patch(0)
		, bIsFork(false)
		, ForkMajor(0)
		, ForkMinor(0)
		, ForkPatch(0)
		, bHasCatFileWithFilters(false)
		, bHasGitLfs(false)
		, bHasGitLfsLocking(false)
	{
	}

	inline bool IsGreaterOrEqualThan(int InMajor, int InMinor) const
	{
		return (Major > InMajor) || (Major == InMajor && (Minor >= InMinor));
	}
};

class FGitSourceControlProvider : public ISourceControlProvider
{
public:
	/** Constructor */
	FGitSourceControlProvider() 
		: bGitAvailable(false)
		, bGitRepositoryFound(false)
	{
	}

	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName(void) const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override;
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn) override;
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override;
	virtual ECommandResult::Type GetState( const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage ) override;
#if ENGINE_MAJOR_VERSION >= 5
        virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
#endif
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
#if ENGINE_MAJOR_VERSION < 5
	virtual ECommandResult::Type Execute( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation ) const override;
	virtual void CancelOperation( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation ) override;
#else
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual void CancelOperation( const FSourceControlOperationRef& InOperation ) override;
#endif
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesCheckout() const override;
	virtual void Tick() override;
	virtual TArray< TSharedRef<class ISourceControlLabel> > GetLabels( const FString& InMatchingSpec ) const override;

#if ENGINE_MAJOR_VERSION >= 5
	virtual TArray<FSourceControlChangelistRef> GetChangelists( EStateCacheUsage::Type InStateCacheUsage ) override;
#endif

#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	/**
	 * Check configuration, else standard paths, and run a Git "version" command to check the availability of the binary.
	 */
	void CheckGitAvailability();

	/** Refresh Git settings from source control settings */
	void UpdateSettings();

	/**
	 * Find the .git/ repository and check it's status.
	 */
	void CheckRepositoryStatus(const FString& InPathToGitBinary);

	/** Is git binary found and working. */
	inline bool IsGitAvailable() const
	{
		return bGitAvailable;
	}

	/** Git version for feature checking */
	inline const FGitVersion& GetGitVersion() const
	{
		return GitVersion;
	}

	/** Get the path to the root of the Git repository: can be the ProjectDir itself, or any parent directory */
	inline const FString& GetPathToRepositoryRoot() const
	{
		return PathToRepositoryRoot;
	}

	/** Gets the path to the Git binary */
	inline const FString& GetGitBinaryPath() const
	{
		return PathToGitBinary;
	}

	/** Git config user.name */
	inline const FString& GetUserName() const
	{
		return UserName;
	}

	/** Git config user.email */
	inline const FString& GetUserEmail() const
	{
		return UserEmail;
	}

	/** Git remote origin url */
	inline const FString& GetRemoteUrl() const
	{
		return RemoteUrl;
	}

	inline const FString& GetLockUser() const
	{
		return LockUser;
	}

	/** Helper function used to update state cache */
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& Filename);

	/**
	 * Register a worker with the provider.
	 * This is used internally so the provider can maintain a map of all available operations.
	 */
	void RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate );

	/** Set list of error messages that occurred after last perforce command */
	void SetLastErrors(const TArray<FText>& InErrors);

	/** Get list of error messages that occurred after last perforce command */
	TArray<FText> GetLastErrors() const;

	/** Get number of error messages seen after running last perforce command */
	int32 GetNumLastErrors() const;

	/** Remove a named file from the state cache */
	bool RemoveFileFromCache(const FString& Filename);

	/** Get files in cache */
	TArray<FString> GetFilesInCache();

	bool AddFileToIgnoreForceCache(const FString& Filename);

	bool RemoveFileFromIgnoreForceCache(const FString& Filename);

	const FString& GetBranchName() const
	{
		return BranchName;
	}

	const FString& GetRemoteBranchName() const { return RemoteBranchName; }

	const TArray<FString>& GetStatusBranchNames() const
	{
		return StatusBranchNames;
	}

	/** Indicates editor binaries are to be updated upon next sync */
	bool bPendingRestart;

#if ENGINE_MAJOR_VERSION >= 5
	uint32 TicksUntilNextForcedUpdate = 0;
#endif

private:
	/** Is git binary found and working. */
	bool bGitAvailable;

	/** Is git repository found. */
	bool bGitRepositoryFound;

	/** Is LFS locking enabled? */
	bool bUsingGitLfsLocking = false;

	FString PathToGitBinary;

	FString LockUser;

	/** Critical section for thread safety of error messages that occurred after last perforce command */
	mutable FCriticalSection LastErrorsCriticalSection;

	/** List of error messages that occurred after last perforce command */
	TArray<FText> LastErrors;

	/** Helper function for Execute() */
	TSharedPtr<class IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;

	/** Helper function for running command synchronously. */
	ECommandResult::Type ExecuteSynchronousCommand(class FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg);
	/** Issue a command asynchronously if possible. */
	ECommandResult::Type IssueCommand(class FGitSourceControlCommand& InCommand, const bool bSynchronous = false );

	/** Output any messages this command holds */
	void OutputCommandMessages(const class FGitSourceControlCommand& InCommand) const;

	/** Update repository status on Connect and UpdateStatus operations */
	void UpdateRepositoryStatus(const class FGitSourceControlCommand& InCommand);

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToRepositoryRoot;

	/** Git config user.name (from local repository, else globally) */
	FString UserName;

	/** Git config user.email (from local repository, else globally) */
	FString UserEmail;

	/** Name of the current branch */
	FString BranchName;

	/** Name of the current remote branch */
	FString RemoteBranchName;

	/** URL of the "origin" default remote server */
	FString RemoteUrl;

	/** Current Commit full SHA1 */
	FString CommitId;

	/** Current Commit description's Summary */
	FString CommitSummary;

	/** State cache */
	TMap<FString, TSharedRef<class FGitSourceControlState, ESPMode::ThreadSafe> > StateCache;

	/** The currently registered source control operations */
	TMap<FName, FGetGitSourceControlWorker> WorkersMap;

	/** Queue for commands given by the main thread */
	TArray < FGitSourceControlCommand* > CommandQueue;

	/** For notifying when the source control states in the cache have changed */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Git version for feature checking */
	FGitVersion GitVersion;

	/** Source Control Menu Extension */
	FGitSourceControlMenu GitSourceControlMenu;

	/**
		Ignore these files when forcing status updates. We add to this list when we've just updated the status already.
		UE4's SourceControl has a habit of performing a double status update, immediately after an operation.
	*/
	TArray<FString> IgnoreForceCache;

	/** Array of branch names for status queries */
	TArray<FString> StatusBranchNames;

	class FGitSourceControlRunner* Runner = nullptr;
};
