// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

#include "LevelEditor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/MessageDialog.h"
#include "EditorStyleSet.h"

#include "PackageTools.h"
#include "FileHelpers.h"

#include "Logging/MessageLog.h"
#include "SourceControlHelpers.h"
#include "SourceControlWindows/Public/SourceControlWindows.h"

static const FName GitSourceControlMenuTabName(TEXT("GitSourceControlMenu"));

#define LOCTEXT_NAMESPACE "GitSourceControl"

TWeakPtr<SNotificationItem> FGitSourceControlMenu::OperationInProgressNotification;

void FGitSourceControlMenu::Register()
{
	// Register the extension with the level editor
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditorModule)
	{
	#if ENGINE_MAJOR_VERSION < 5
		FLevelEditorModule::FLevelEditorMenuExtender ViewMenuExtender = FLevelEditorModule::FLevelEditorMenuExtender::CreateRaw(this, &FGitSourceControlMenu::OnExtendLevelEditorViewMenu);
		auto& MenuExtenders = LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders();
		MenuExtenders.Add(ViewMenuExtender);
		ViewMenuExtenderHandle = MenuExtenders.Last().GetHandle();
	#else
		auto MakeMenu = [this](FMenuBarBuilder& Builder)
		{
			Builder.AddPullDownMenu(
				NSLOCTEXT("Git", "Git", "Git"),
				NSLOCTEXT("Git", "GitMenu_ToolTip", "Open the Git menu"),
				FNewMenuDelegate::CreateRaw(this, &FGitSourceControlMenu::AddMenuExtension)
			);
		};
		
		ViewMenuExtender = MakeShareable(new FExtender);
		ViewMenuExtender->AddMenuBarExtension("Window", EExtensionHook::After, nullptr, FMenuBarExtensionDelegate::CreateLambda(MakeMenu));
		
		LevelEditorModule->GetMenuExtensibilityManager()->AddExtender(ViewMenuExtender);
	#endif
	}
}

void FGitSourceControlMenu::Unregister()
{
	// Unregister the level editor extensions
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
	#if ENGINE_MAJOR_VERSION < 5
		LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll([=](const FLevelEditorModule::FLevelEditorMenuExtender& Extender) { return Extender.GetHandle() == ViewMenuExtenderHandle; });
	#else
		LevelEditorModule->GetMenuExtensibilityManager()->RemoveExtender(ViewMenuExtender);
	#endif
	}
}

bool FGitSourceControlMenu::HaveRemoteUrl() const
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	return !GitSourceControl.GetProvider().GetRemoteUrl().IsEmpty();
}

/// Prompt to save or discard all packages
bool FGitSourceControlMenu::SaveDirtyPackages()
{
	const bool bPromptUserToSave = true;
	const bool bSaveMapPackages = true;
	const bool bSaveContentPackages = true;
	const bool bFastSave = false;
	const bool bNotifyNoPackagesSaved = false;
	const bool bCanBeDeclined = true; // If the user clicks "don't save" this will continue and lose their changes
	bool bHadPackagesToSave = false;

	bool bSaved = FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages, bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined, &bHadPackagesToSave);

	// bSaved can be true if the user selects to not save an asset by unchecking it and clicking "save"
	if (bSaved)
	{
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
		FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
		bSaved = DirtyPackages.Num() == 0;
	}

	return bSaved;
}

// Ask the user if they want to stash any modification and try to unstash them afterward, which could lead to conflicts
bool FGitSourceControlMenu::StashAwayAnyModifications()
{
	bool bStashOk = true;

	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const FString& PathToRespositoryRoot = Provider.GetPathToRepositoryRoot();
	const FString& PathToGitBinary = Provider.GetGitBinaryPath();
	const TArray<FString> ParametersStatus{"--porcelain --untracked-files=no"};
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	// Check if there is any modification to the working tree
	const bool bStatusOk = GitSourceControlUtils::RunCommand(TEXT("status"), PathToGitBinary, PathToRespositoryRoot, ParametersStatus, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if ((bStatusOk) && (InfoMessages.Num() > 0))
	{
		// Ask the user before stashing
		const FText DialogText(LOCTEXT("SourceControlMenu_Stash_Ask", "Stash (save) all modifications of the working tree? Required to Sync/Pull!"));
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
		if (Choice == EAppReturnType::Ok)
		{
			const TArray<FString> ParametersStash{ "save \"Stashed by Unreal Engine Git Plugin\"" };
			bStashMadeBeforeSync = GitSourceControlUtils::RunCommand(TEXT("stash"), PathToGitBinary, PathToRespositoryRoot, ParametersStash, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
			if (!bStashMadeBeforeSync)
			{
				FMessageLog SourceControlLog("SourceControl");
				SourceControlLog.Warning(LOCTEXT("SourceControlMenu_StashFailed", "Stashing away modifications failed!"));
				SourceControlLog.Notify();
			}
		}
		else
		{
			bStashOk = false;
		}
	}

	return bStashOk;
}

// Unstash any modifications if a stash was made at the beginning of the Sync operation
void FGitSourceControlMenu::ReApplyStashedModifications()
{
	if (bStashMadeBeforeSync)
	{
		FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		const FString& PathToRespositoryRoot = Provider.GetPathToRepositoryRoot();
		const FString& PathToGitBinary = Provider.GetGitBinaryPath();
		const TArray<FString> ParametersStash{ "pop" };
		TArray<FString> InfoMessages;
		TArray<FString> ErrorMessages;
		const bool bUnstashOk = GitSourceControlUtils::RunCommand(TEXT("stash"), PathToGitBinary, PathToRespositoryRoot, ParametersStash, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
		if (!bUnstashOk)
		{
			FMessageLog SourceControlLog("SourceControl");
			SourceControlLog.Warning(LOCTEXT("SourceControlMenu_UnstashFailed", "Unstashing previously saved modifications failed!"));
			SourceControlLog.Notify();
		}
	}
}

void FGitSourceControlMenu::SyncClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Ask the user to save any dirty assets opened in Editor
		const bool bSaved = SaveDirtyPackages();
		if (bSaved)
		{
			FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
			FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

			// Launch a "Sync" operation
			TSharedRef<FSync, ESPMode::ThreadSafe> SyncOperation = ISourceControlOperation::Create<FSync>();
			const ECommandResult::Type Result = Provider.Execute(SyncOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
																 FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
			if (Result == ECommandResult::Succeeded)
			{
				// Display an ongoing notification during the whole operation (packages will be reloaded at the completion of the operation)
				DisplayInProgressNotification(SyncOperation->GetInProgressString());
			}
			else
			{
				// Report failure with a notification and Reload all packages
				DisplayFailureNotification(SyncOperation->GetName());
			}
		}
		else
		{
			FMessageLog SourceControlLog("SourceControl");
			SourceControlLog.Warning(LOCTEXT("SourceControlMenu_Sync_Unsaved", "Save All Assets before attempting to Sync!"));
			SourceControlLog.Notify();
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

void FGitSourceControlMenu::CommitClicked()
{
	if (OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}
	
	FLevelEditorModule & LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	FSourceControlWindows::ChoosePackagesToCheckIn(nullptr);
}

void FGitSourceControlMenu::PushClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch a "Push" Operation
		FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		TSharedRef<FCheckIn, ESPMode::ThreadSafe> PushOperation = ISourceControlOperation::Create<FCheckIn>();
		const ECommandResult::Type Result = Provider.Execute(PushOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(PushOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(PushOperation->GetName());
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

void FGitSourceControlMenu::RevertClicked()
{
	if (OperationInProgressNotification.IsValid())
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
		return;
	}

	// Ask the user before reverting all!
	const FText DialogText(LOCTEXT("SourceControlMenu_Revert_Ask", "Revert all modifications of the working tree?"));
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
	if (Choice != EAppReturnType::Ok)
	{
		return;
	}

	// make sure we update the SCC status of all packages (this could take a long time, so we will run it as a background task)
	const TArray<FString> Filenames {
		FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()),
		FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
	};

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	FSourceControlOperationRef Operation = ISourceControlOperation::Create<FUpdateStatus>();
	SourceControlProvider.Execute(Operation, Filenames, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateStatic(&FGitSourceControlMenu::RevertAllCallback));

	FNotificationInfo Info(LOCTEXT("SourceControlMenuRevertAll", "Checking for assets to revert..."));
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.0f;
	Info.FadeOutDuration = 1.0f;

	if (SourceControlProvider.CanCancelOperation(Operation))
	{
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("SourceControlMenuRevertAll_CancelButton", "Cancel"),
			LOCTEXT("SourceControlMenuRevertAll_CancelButtonTooltip", "Cancel the revert operation."),
			FSimpleDelegate::CreateStatic(&FGitSourceControlMenu::RevertAllCancelled, Operation)
		));
	}

	OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
	}
}

void FGitSourceControlMenu::RevertAllCallback(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	if (InResult != ECommandResult::Succeeded)
	{
		return;
	}

	// Get a list of all the checked out packages
	TArray<FString> PackageNames;
	TArray<UPackage*> LoadedPackages;
	TMap<FString, FSourceControlStatePtr> PackageStates;
	FEditorFileUtils::FindAllSubmittablePackageFiles(PackageStates, true);

	for (TMap<FString, FSourceControlStatePtr>::TConstIterator PackageIter(PackageStates); PackageIter; ++PackageIter)
	{
		const FString PackageName = *PackageIter.Key();
		const FSourceControlStatePtr CurPackageSCCState = PackageIter.Value();

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package != nullptr)
		{
			LoadedPackages.Add(Package);

			if (!Package->IsFullyLoaded())
			{
				FlushAsyncLoading();
				Package->FullyLoad();
			}
			ResetLoaders(Package);
		}

		PackageNames.Add(PackageName);
	}

	const auto FileNames = SourceControlHelpers::PackageFilenames(PackageNames);

	// Launch a "Revert" Operation
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOperation = ISourceControlOperation::Create<FRevert>();
	const auto Result = Provider.Execute(RevertOperation, FileNames);

	RemoveInProgressNotification();
	if (Result != ECommandResult::Succeeded)
	{
		DisplayFailureNotification(TEXT("Revert"));
	}
	else
	{
		DisplaySucessNotification(TEXT("Revert"));
	}

	GitSourceControlUtils::ReloadPackages(LoadedPackages);
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous);
}

void FGitSourceControlMenu::RefreshClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		// Launch an "GitFetch" Operation
		TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FGitFetch>();
		RefreshOperation->bUpdateStatus = true;
		const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FGitSourceControlModule::GetEmptyStringArray(), EConcurrency::Asynchronous,
															 FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RefreshOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RefreshOperation->GetName());
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

// Display an ongoing notification during the whole operation
void FGitSourceControlMenu::DisplayInProgressNotification(const FText& InOperationInProgressString)
{
	if (!OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InOperationInProgressString);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (OperationInProgressNotification.IsValid())
		{
			OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

void FGitSourceControlMenu::RevertAllCancelled(FSourceControlOperationRef InOperation)
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	SourceControlProvider.CancelOperation(InOperation);

	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
	}

	OperationInProgressNotification.Reset();
}

// Remove the ongoing notification at the end of the operation
void FGitSourceControlMenu::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void FGitSourceControlMenu::DisplaySucessNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Success", "{0} operation was successful!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
	Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
#if UE_BUILD_DEBUG
	UE_LOG(LogSourceControl, Log, TEXT("%s"), *NotificationText.ToString());
#endif
}

// Display a temporary failure notification at the end of the operation
void FGitSourceControlMenu::DisplayFailureNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Failure", "Error: {0} operation failed!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	UE_LOG(LogSourceControl, Error, TEXT("%s"), *NotificationText.ToString());
}

void FGitSourceControlMenu::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	if ((InOperation->GetName() == "Sync") || (InOperation->GetName() == "Revert"))
	{
		// Unstash any modifications if a stash was made at the beginning of the Sync operation
		ReApplyStashedModifications();
		// Reload packages that where unlinked at the beginning of the Sync/Revert operation
		GitSourceControlUtils::ReloadPackages(PackagesToReload);
	}

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySucessNotification(InOperation->GetName());
	}
	else
	{
		DisplayFailureNotification(InOperation->GetName());
	}
}

void FGitSourceControlMenu::AddMenuExtension(FMenuBuilder& Builder)
{
#if ENGINE_MAJOR_VERSION >= 5
	Builder.AddMenuEntry(
		LOCTEXT("GitCommit", "Submit to Source Control"),
		LOCTEXT("GitPushTooltip", "Commits all local pending changes"),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Submit"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::CommitClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::HaveRemoteUrl)
		)
	);
	
	Builder.AddSeparator("GitSourceControlPluginActions");
#endif

	Builder.AddMenuEntry(
		LOCTEXT("GitPush",				"Push pending local commits"),
		LOCTEXT("GitPushTooltip",		"Push all pending local commits to the remote server."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Submit"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::PushClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::HaveRemoteUrl)
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("GitSync",				"Pull"),
		LOCTEXT("GitSyncTooltip",		"Update all files in the local repository to the latest version of the remote server."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Sync"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::SyncClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::HaveRemoteUrl)
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("GitRevert",			"Revert"),
		LOCTEXT("GitRevertTooltip",		"Revert all files in the repository to their unchanged state."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RevertClicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("GitRefresh",			"Refresh"),
		LOCTEXT("GitRefreshTooltip",	"Update the source control status of all files in the local repository."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Refresh"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshClicked),
			FCanExecuteAction()
		)
	);
}

TSharedRef<FExtender> FGitSourceControlMenu::OnExtendLevelEditorViewMenu(const TSharedRef<FUICommandList> CommandList)
{
	TSharedRef<FExtender> Extender(new FExtender());

	Extender->AddMenuExtension(
		"SourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FGitSourceControlMenu::AddMenuExtension));

	return Extender;
}

#undef LOCTEXT_NAMESPACE
