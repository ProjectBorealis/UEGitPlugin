// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlProvider.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"
#include "Modules/ModuleManager.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "GitSourceControlCommand.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "SGitSourceControlSettings.h"
#include "GitSourceControlRunner.h"
#include "Logging/MessageLog.h"
#include "ScopedSourceControlProgress.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

static FName ProviderName("Git LFS 2");

void FGitSourceControlProvider::Init(bool bForceConnection)
{
	// Init() is called multiple times at startup: do not check git each time
	if(!bGitAvailable)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
		if(Plugin.IsValid())
		{
			UE_LOG(LogSourceControl, Log, TEXT("Git plugin '%s'"), *(Plugin->GetDescriptor().VersionName));
		}

		CheckGitAvailability();
	}

	// bForceConnection: not used anymore
}

void FGitSourceControlProvider::CheckGitAvailability()
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	if(PathToGitBinary.IsEmpty())
	{
		// Try to find Git binary, and update settings accordingly
		PathToGitBinary = GitSourceControlUtils::FindGitBinaryPath();
		if(!PathToGitBinary.IsEmpty())
		{
			GitSourceControl.AccessSettings().SetBinaryPath(PathToGitBinary);
		}
	}

	if(!PathToGitBinary.IsEmpty())
	{
		UE_LOG(LogSourceControl, Log, TEXT("Using '%s'"), *PathToGitBinary);
		bGitAvailable = GitSourceControlUtils::CheckGitAvailability(PathToGitBinary, &GitVersion);
		if(bGitAvailable)
		{
			CheckRepositoryStatus(PathToGitBinary);
		}
	}
	else
	{
		bGitAvailable = false;
	}
}

void FGitSourceControlProvider::UpdateSettings()
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	bUsingGitLfsLocking = GitSourceControl.AccessSettings().IsUsingGitLfsLocking();
	LockUser = GitSourceControl.AccessSettings().GetLfsUserName();
}

void FGitSourceControlProvider::CheckRepositoryStatus(const FString& InPathToGitBinary)
{
	// Find the path to the root Git directory (if any, else uses the ProjectDir)
	const FString PathToProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	bGitRepositoryFound = GitSourceControlUtils::FindRootDirectory(PathToProjectDir, PathToRepositoryRoot);
	if(bGitRepositoryFound)
	{
		GitSourceControlMenu.Register();

		// Get branch name
		bGitRepositoryFound = GitSourceControlUtils::GetBranchName(InPathToGitBinary, PathToRepositoryRoot, BranchName);
		if(bGitRepositoryFound)
		{
			GitSourceControlUtils::GetRemoteBranchName(InPathToGitBinary, PathToRepositoryRoot, RemoteBranchName);
			GitSourceControlUtils::GetRemoteUrl(InPathToGitBinary, PathToRepositoryRoot, RemoteUrl);
			UpdateSettings();
			TArray<FString> Files {TEXT("*.uasset"), TEXT("*.umap")};
			TArray<FString> ErrorMessages;
			if (!GitSourceControlUtils::CheckLFSLockable(InPathToGitBinary, PathToRepositoryRoot, Files, ErrorMessages))
			{
				for (const auto& ErrorMessage : ErrorMessages)
				{
					UE_LOG(LogSourceControl, Error, TEXT("%s"), *ErrorMessage);
				}
			}
			const TArray<FString> ProjectDirs {FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
											   FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
											   FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())};
			ErrorMessages.Empty();
			TMap<FString, FGitSourceControlState> States;
			if (GitSourceControlUtils::RunUpdateStatus(InPathToGitBinary, PathToRepositoryRoot, bUsingGitLfsLocking, ProjectDirs, ErrorMessages, States))
			{
				TMap<const FString, FGitState> Results;
				if (GitSourceControlUtils::CollectNewStates(States, Results))
				{
					GitSourceControlUtils::UpdateCachedStates(Results);
				}
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Failed to update repo on initialization."));
			}
			Runner = new FGitSourceControlRunner();
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("'%s' is not a valid Git repository"), *PathToRepositoryRoot);
		}
	}
	else
	{
		UE_LOG(LogSourceControl, Warning, TEXT("'%s' is not part of a Git repository"), *FPaths::ProjectDir());
	}

	// Get user name & email (of the repository, else from the global Git config)
	GitSourceControlUtils::GetUserConfig(InPathToGitBinary, PathToRepositoryRoot, UserName, UserEmail);
}

void FGitSourceControlProvider::SetLastErrors(const TArray<FText>& InErrors)
{

	FScopeLock Lock(&LastErrorsCriticalSection);
	LastErrors = InErrors;
}

TArray<FText> FGitSourceControlProvider::GetLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	TArray<FText> Result = LastErrors;
	return Result;
}

int32 FGitSourceControlProvider::GetNumLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	return LastErrors.Num();
}

void FGitSourceControlProvider::Close()
{
	// clear the cache
	StateCache.Empty();
	// Remove all extensions to the "Source Control" menu in the Editor Toolbar
	GitSourceControlMenu.Unregister();

	bGitAvailable = false;
	bGitRepositoryFound = false;
	UserName.Empty();
	UserEmail.Empty();
	if (Runner)
	{
		delete Runner;
		Runner = nullptr;
	}
}

TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> FGitSourceControlProvider::GetStateInternal(const FString& Filename)
{
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(Filename);
	if (State != NULL)
	{
		// found cached item
		return (*State);
	}
	else
	{
		// cache an unknown state for this item
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> NewState = MakeShareable( new FGitSourceControlState(Filename) );
		StateCache.Add(Filename, NewState);
		return NewState;
	}
}

FText FGitSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("IsAvailable"), (IsEnabled() && IsAvailable()) ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add( TEXT("RepositoryName"), FText::FromString(PathToRepositoryRoot) );
	Args.Add( TEXT("RemoteUrl"), FText::FromString(RemoteUrl) );
	Args.Add( TEXT("UserName"), FText::FromString(UserName) );
	Args.Add( TEXT("UserEmail"), FText::FromString(UserEmail) );
	Args.Add( TEXT("BranchName"), FText::FromString(BranchName) );
	Args.Add( TEXT("CommitId"), FText::FromString(CommitId.Left(8)) );
	Args.Add( TEXT("CommitSummary"), FText::FromString(CommitSummary) );

	FText FormattedError;
	const TArray<FText>& RecentErrors = GetLastErrors();
	if (RecentErrors.Num() > 0)
	{
		FFormatNamedArguments ErrorArgs;
		ErrorArgs.Add(TEXT("ErrorText"), RecentErrors[0]);

		FormattedError = FText::Format(LOCTEXT("GitErrorStatusText", "Error: {ErrorText}\n\n"), ErrorArgs);
	}

	Args.Add(TEXT("ErrorText"), FormattedError);

	return FText::Format( NSLOCTEXT("GitStatusText", "{ErrorText}Enabled: {IsAvailable}", "Local repository: {RepositoryName}\nRemote: {RemoteUrl}\nUser: {UserName}\nE-mail: {UserEmail}\n[{BranchName} {CommitId}] {CommitSummary}"), Args );
}

/** Quick check if source control is enabled */
bool FGitSourceControlProvider::IsEnabled() const
{
	return bGitRepositoryFound;
}

/** Quick check if source control is available for use (useful for server-based providers) */
bool FGitSourceControlProvider::IsAvailable() const
{
	return bGitRepositoryFound;
}

const FName& FGitSourceControlProvider::GetName(void) const
{
	return ProviderName;
}

ECommandResult::Type FGitSourceControlProvider::GetState( const TArray<FString>& InFiles, TArray< TSharedRef<ISourceControlState, ESPMode::ThreadSafe> >& OutState, EStateCacheUsage::Type InStateCacheUsage )
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		TArray<FString> ForceUpdate;
		for (FString Path : InFiles)
		{
			// Remove the path from the cache, so it's not ignored the next time we force check.
			// If the file isn't in the cache, force update it now.
			if (!RemoveFileFromIgnoreForceCache(Path))
			{
				ForceUpdate.Add(Path);
			}
		}
		if (ForceUpdate.Num() > 0)
		{
			Execute(ISourceControlOperation::Create<FUpdateStatus>(), ForceUpdate);
		}
	}

	const TArray<FString>& AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	for (TArray<FString>::TConstIterator It(AbsoluteFiles); It; It++)
	{
		OutState.Add(GetStateInternal(*It));
	}

	return ECommandResult::Succeeded;
}

#if ENGINE_MAJOR_VERSION >= 5
ECommandResult::Type FGitSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
    return ECommandResult::Failed;
}
#endif

TArray<FSourceControlStateRef> FGitSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		const FSourceControlStateRef& State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

bool FGitSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	return StateCache.Remove(Filename) > 0;
}

bool FGitSourceControlProvider::AddFileToIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Add(Filename) > 0;
}

bool FGitSourceControlProvider::RemoveFileFromIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Remove(Filename) > 0;
}

/** Get files in cache */
TArray<FString> FGitSourceControlProvider::GetFilesInCache()
{
	TArray<FString> Files;
	for (const auto& State : StateCache)
	{
		Files.Add(State.Key);
	}
	return Files;
}

FDelegateHandle FGitSourceControlProvider::RegisterSourceControlStateChanged_Handle( const FSourceControlStateChanged::FDelegate& SourceControlStateChanged )
{
	return OnSourceControlStateChanged.Add( SourceControlStateChanged );
}

void FGitSourceControlProvider::UnregisterSourceControlStateChanged_Handle( FDelegateHandle Handle )
{
	OnSourceControlStateChanged.Remove( Handle );
}

#if ENGINE_MAJOR_VERSION < 5
ECommandResult::Type FGitSourceControlProvider::Execute( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#else
ECommandResult::Type FGitSourceControlProvider::Execute( const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#endif
{
	if(!IsEnabled() && !(InOperation->GetName() == "Connect")) // Only Connect operation allowed while not Enabled (Repository found)
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	const TArray<FString>& AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	// Query to see if we allow this operation
	TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if(!Worker.IsValid())
	{
		// this operation is unsupported by this source control provider
		FFormatNamedArguments Arguments;
		Arguments.Add( TEXT("OperationName"), FText::FromName(InOperation->GetName()) );
		Arguments.Add( TEXT("ProviderName"), FText::FromName(GetName()) );
		FText Message(FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by source control provider '{ProviderName}'"), Arguments));

		FMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);

		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FGitSourceControlCommand* Command = new FGitSourceControlCommand(InOperation, Worker.ToSharedRef());
	Command->Files = AbsoluteFiles;
	Command->OperationCompleteDelegate = InOperationCompleteDelegate;

	// fire off operation
	if(InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("ExecuteSynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString(), false);
	}
	else
	{
		Command->bAutoDelete = true;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("IssueAsynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return IssueCommand(*Command);
	}
}

#if ENGINE_MAJOR_VERSION < 5
bool FGitSourceControlProvider::CanCancelOperation( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation ) const
#else
bool FGitSourceControlProvider::CanCancelOperation( const FSourceControlOperationRef& InOperation ) const
#endif
{
	// TODO: maybe support cancellation again?
#if 0
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		const FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			return true;
		}
	}
#endif

	// operation was not in progress!
	return false;
}

#if ENGINE_MAJOR_VERSION < 5
void FGitSourceControlProvider::CancelOperation( const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation )
#else
void FGitSourceControlProvider::CancelOperation( const FSourceControlOperationRef& InOperation )
#endif
{
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			Command.Cancel();
			return;
		}
	}
}

bool FGitSourceControlProvider::UsesLocalReadOnlyState() const
{
	return bUsingGitLfsLocking; // Git LFS Lock uses read-only state
}

bool FGitSourceControlProvider::UsesChangelists() const
{
	return false;
}

bool FGitSourceControlProvider::UsesCheckout() const
{
	return bUsingGitLfsLocking; // Git LFS Lock uses read-only state
}

TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> FGitSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	const FGetGitSourceControlWorker* Operation = WorkersMap.Find(InOperationName);
	if(Operation != nullptr)
	{
		return Operation->Execute();
	}

	return nullptr;
}

void FGitSourceControlProvider::RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate )
{
	WorkersMap.Add( InName, InDelegate );
}

void FGitSourceControlProvider::OutputCommandMessages(const FGitSourceControlCommand& InCommand) const
{
	FMessageLog SourceControlLog("SourceControl");

	for (int32 ErrorIndex = 0; ErrorIndex < InCommand.ResultInfo.ErrorMessages.Num(); ++ErrorIndex)
	{
		SourceControlLog.Error(FText::FromString(InCommand.ResultInfo.ErrorMessages[ErrorIndex]));
	}

	for (int32 InfoIndex = 0; InfoIndex < InCommand.ResultInfo.InfoMessages.Num(); ++InfoIndex)
	{
		SourceControlLog.Info(FText::FromString(InCommand.ResultInfo.InfoMessages[InfoIndex]));
	}
}

void FGitSourceControlProvider::UpdateRepositoryStatus(const class FGitSourceControlCommand& InCommand)
{
	// For all operations running UpdateStatus, get Commit information:
	if (!InCommand.CommitId.IsEmpty())
	{
		CommitId = InCommand.CommitId;
		CommitSummary = InCommand.CommitSummary;
	}
}

void FGitSourceControlProvider::Tick()
{
#if ENGINE_MAJOR_VERSION < 5
	bool bStatesUpdated = false;
#else
	bool bStatesUpdated = TicksUntilNextForcedUpdate == 1;
	if( TicksUntilNextForcedUpdate > 0 )
	{
		--TicksUntilNextForcedUpdate;
	}
#endif

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];

		if (Command.bExecuteProcessed)
		{
			// Remove command from the queue
			CommandQueue.RemoveAt(CommandIndex);

			if (!Command.IsCanceled())
			{
				// Update repository status on UpdateStatus operations
				UpdateRepositoryStatus(Command);
			}

			// let command update the states of any files
			bStatesUpdated |= Command.Worker->UpdateStates();

			// dump any messages to output log
			OutputCommandMessages(Command);

			// run the completion delegate callback if we have one bound
			if (!Command.IsCanceled())
			{
				Command.ReturnResults();
			}

			// commands that are left in the array during a tick need to be deleted
			if(Command.bAutoDelete)
			{
				// Only delete commands that are not running 'synchronously'
				delete &Command;
			}

			// only do one command per tick loop, as we dont want concurrent modification
			// of the command queue (which can happen in the completion delegate)
			break;
		}
		else if (Command.bCancelled)
		{
			// If this was a synchronous command, set it free so that it will be deleted automatically
			// when its (still running) thread finally finishes
			Command.bAutoDelete = true;

			Command.ReturnResults();
			break;
		}
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TArray< TSharedRef<ISourceControlLabel> > FGitSourceControlProvider::GetLabels( const FString& InMatchingSpec ) const
{
	TArray< TSharedRef<ISourceControlLabel> > Tags;

	// NOTE list labels. Called by CrashDebugHelper() (to remote debug Engine crash)
	//					 and by SourceControlHelpers::AnnotateFile() (to add source file to report)
	// Reserved for internal use by Epic Games with Perforce only
	return Tags;
}

#if ENGINE_MAJOR_VERSION >= 5
TArray<FSourceControlChangelistRef> FGitSourceControlProvider::GetChangelists( EStateCacheUsage::Type InStateCacheUsage )
{
    return TArray<FSourceControlChangelistRef>();
}
#endif

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FGitSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SGitSourceControlSettings);
}
#endif

ECommandResult::Type FGitSourceControlProvider::ExecuteSynchronousCommand(FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	struct Local
	{
		static void CancelCommand(FGitSourceControlCommand* InControlCommand)
		{
			InControlCommand->Cancel();
		}
	};

	FText TaskText = Task;
	// Display the progress dialog
	if (bSuppressResponseMsg)
	{
		TaskText = FText::GetEmpty();
	}
	
	int i = 0;

	// Display the progress dialog if a string was provided
	{
		// TODO: support cancellation?
		//FScopedSourceControlProgress Progress(TaskText, FSimpleDelegate::CreateStatic(&Local::CancelCommand, &InCommand));
		FScopedSourceControlProgress Progress(TaskText);
		
		// Issue the command asynchronously...
		IssueCommand( InCommand );

		// ... then wait for its completion (thus making it synchronous)
		while (!InCommand.IsCanceled() && CommandQueue.Contains(&InCommand))
		{
			// Tick the command queue and update progress.
			Tick();

			if (i >= 20) {
				Progress.Tick();
				i = 0;
			}
			i++;

			// Sleep so we don't busy-wait so much.
			FPlatformProcess::Sleep(0.01f);
		}

		if (InCommand.bCancelled)
		{
			Result = ECommandResult::Cancelled;
		}
		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
		else if (!bSuppressResponseMsg)
		{
			FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("Git_ServerUnresponsive", "Git command failed. Please check your connection and try again, or check the output log for more information.") );
			UE_LOG(LogSourceControl, Error, TEXT("Command '%s' Failed!"), *InCommand.Operation->GetName().ToString());
		}
	}

	// Delete the command now if not marked as auto-delete
	if (!InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

ECommandResult::Type FGitSourceControlProvider::IssueCommand(FGitSourceControlCommand& InCommand, const bool bSynchronous)
{
	if (!bSynchronous && GThreadPool != nullptr)
	{
		// Queue this to our worker thread(s) for resolving.
		// When asynchronous, any callback gets called from Tick().
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}
	else
	{
		UE_LOG(LogSourceControl, Log, TEXT("There are no threads available to process the source control command '%s'. Running synchronously."), *InCommand.Operation->GetName().ToString());

		InCommand.bCommandSuccessful = InCommand.DoWork();

		InCommand.Worker->UpdateStates();

		OutputCommandMessages(InCommand);

		// Callback now if present. When asynchronous, this callback gets called from Tick().
		return InCommand.ReturnResults();
	}
}

bool FGitSourceControlProvider::QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest)
{
	// Check similar preconditions to Perforce (valid src and dest), 
	if (ConfigSrc.Len() == 0 || ConfigDest.Len() == 0)
	{
		return false;
	}

	if (!bGitAvailable || !bGitRepositoryFound)
	{
		FMessageLog("SourceControl").Error(LOCTEXT("StatusBranchConfigNoConnection", "Unable to retrieve status branch configuration from repo, no connection"));
		return false;
	}

	// Otherwise, we can assume that whatever our user is doing to config state branches is properly synced, so just copy.
	// TODO: maybe don't assume, and use git show instead?
	IFileManager::Get().Copy(*ConfigDest, *ConfigSrc);
	return true;
}

void FGitSourceControlProvider::RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn)
{
	StatusBranchNames = BranchNames;
}

int32 FGitSourceControlProvider::GetStateBranchIndex(const FString& StateBranchName) const
{
	// How do state branches indices work?
	// Order matters. Lower values are lower in the hierarchy, i.e., changes from higher branches get automatically merged down.
	// The higher branch is, the stabler it is, and has changes manually promoted up.

	// Check if we are checking the index of the current branch
	// UE uses FEngineVersion for the current branch name because of UEGames setup, but we want to handle otherwise for Git repos.
	if (StateBranchName == FEngineVersion::Current().GetBranch())
	{
		const int32 CurrentBranchStatusIndex = StatusBranchNames.IndexOfByKey(BranchName);
		const bool bCurrentBranchInStatusBranches = CurrentBranchStatusIndex != INDEX_NONE;
		// If the user's current branch is tracked as a status branch, give the proper index
		if (bCurrentBranchInStatusBranches)
		{
			return CurrentBranchStatusIndex;
		}
		// If the current branch is not a status branch, make it the highest branch
		// This is semantically correct, since if a branch is not marked as a status branch
		// it merges changes in a similar fashion to the highest status branch, i.e. manually promotes them
		// based on the user merging those changes in. and these changes always get merged from even the highest point
		// of the stream. i.e, promoted/stable changes are always up for consumption by this branch.
		return INT32_MAX;
	}
	
	// If we're not checking the current branch, then we don't need to do special handling.
	// If it is not a status branch, there is no message
	return StatusBranchNames.IndexOfByKey(StateBranchName);
}

#undef LOCTEXT_NAMESPACE
