// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "AssetToolsModule.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"

#include "GitSourceControlOperations.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/ConfigCacheIni.h"
#include "Runtime/Launch/Resources/Version.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

TArray<FString> FGitSourceControlModule::EmptyStringArray;

namespace
{
    static const FName NAME_SourceControl( TEXT( "SourceControl" ) );
    static const FName NAME_ContentBrowser( TEXT( "ContentBrowser" ) );
}

template<typename Type>
static TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker()
{
	return MakeShareable( new Type() );
}

void FGitSourceControlModule::StartupModule()
{
	// Register our operations (implemented in GitSourceControlOperations.cpp by subclassing from Engine\Source\Developer\SourceControl\Public\SourceControlOperations.h)
	GitSourceControlProvider.RegisterWorker( "Connect", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitConnectWorker> ) );
	// Note: this provider uses the "CheckOut" command only with Git LFS 2 "lock" command, since Git itself has no lock command (all tracked files in the working copy are always already checked-out).
	GitSourceControlProvider.RegisterWorker( "CheckOut", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitCheckOutWorker> ) );
	GitSourceControlProvider.RegisterWorker( "UpdateStatus", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitUpdateStatusWorker> ) );
	GitSourceControlProvider.RegisterWorker( "MarkForAdd", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitMarkForAddWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Delete", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitDeleteWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Revert", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitRevertWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Sync", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitSyncWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Fetch", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitFetchWorker> ) );
	GitSourceControlProvider.RegisterWorker( "CheckIn", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitCheckInWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Copy", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitCopyWorker> ) );
	GitSourceControlProvider.RegisterWorker( "Resolve", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitResolveWorker> ) );
#if ENGINE_MAJOR_VERSION == 5
	GitSourceControlProvider.RegisterWorker( "MoveToChangelist", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitMoveToChangelistWorker> ) );
	GitSourceControlProvider.RegisterWorker( "UpdateChangelistsStatus", FGetGitSourceControlWorker::CreateStatic( &CreateWorker<FGitUpdateStagingWorker> ) );
#endif

	// load our settings
	GitSourceControlSettings.LoadSettings();

	// If configured, do a check if the current user has permissions to access a specified repository. Exit with a fatal error if that is the case.
	FString RequiredRepositoryAccessURL, RequiredRepositoryAccessBranchName;
	GConfig->GetString(TEXT("GitSourceControl"), TEXT("RequiredAccessRepositoryURL"), RequiredRepositoryAccessURL, GEditorIni);
	GConfig->GetString(TEXT("GitSourceControl"), TEXT("RequiredAccessRepositoryBranchName"), RequiredRepositoryAccessBranchName, GEditorIni);
	if (!RequiredRepositoryAccessURL.IsEmpty())
	{
		if (RequiredRepositoryAccessBranchName.IsEmpty())
		{
			RequiredRepositoryAccessBranchName = "main";
		}
		int32 ReturnCode;
		FString StdErr;
		// Will fail (or block forever) over HTTPS if GCM is not set up
		// If using SSH, will fail if user doesn't have SSH keys set up
		const bool bLaunchedProcess = FPlatformProcess::ExecProcess(
			TEXT("git"),
			*FString::Format(TEXT("ls-remote --exit-code {0} {1}"), {RequiredRepositoryAccessURL, RequiredRepositoryAccessBranchName}),
			&ReturnCode, nullptr, &StdErr);
		if (!bLaunchedProcess)
		{
			UE_LOG(LogSourceControl, Fatal, TEXT("Could not launch git: %s"), *StdErr);
		}
		else if (ReturnCode != 0)
		{
			if (StdErr.IsEmpty())
			{
				StdErr = TEXT("Branch not found"); // if there is no output and there is a bad exit code, it's very likely the branch name was not found
			}
			UE_LOG(LogSourceControl, Fatal, TEXT("Could access branch %s on required repository %s(%d): %s"),
				*RequiredRepositoryAccessBranchName, *RequiredRepositoryAccessURL, ReturnCode, *StdErr);
		}
	}

	// Bind our revision control provider to the editor
    IModularFeatures::Get().RegisterModularFeature( NAME_SourceControl, &GitSourceControlProvider );

	FContentBrowserModule & ContentBrowserModule = FModuleManager::Get().LoadModuleChecked< FContentBrowserModule >( NAME_ContentBrowser );

#if ENGINE_MAJOR_VERSION >= 5
	// Register ContentBrowserDelegate Handles for UE5 EA
	// At the time of writing this UE5 is in Early Access and has no support for revision control yet. So instead we hook into the content browser..
	// .. and force a state update on the next tick for revision control. Usually the contentbrowser assets will request this themselves, but that's not working
	// Values here are 1 or 2 based on whether the change can be done immediately or needs to be delayed as unreal needs to work through its internal delegates first
	// >> Technically you wouldn't need to use `GetOnAssetSelectionChanged` -- but it's there as a safety mechanism. States aren't forceupdated for the first path that loads
	// >> Making sure we force an update on selection change that acts like a just in case other measures fail
	CbdHandle_OnFilterChanged = ContentBrowserModule.GetOnFilterChanged().AddLambda( [this]( const FARFilter&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
	CbdHandle_OnSearchBoxChanged = ContentBrowserModule.GetOnSearchBoxChanged().AddLambda( [this]( const FText&, bool ){ GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetSelectionChanged = ContentBrowserModule.GetOnAssetSelectionChanged().AddLambda( [this]( const TArray<FAssetData>&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetPathChanged = ContentBrowserModule.GetOnAssetPathChanged().AddLambda( [this]( const FString& ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
#endif

	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBAssetMenuExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	CBAssetMenuExtenderDelegates.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw( this, &FGitSourceControlModule::OnExtendContentBrowserAssetSelectionMenu ));
	CbdHandle_OnExtendAssetSelectionMenu = CBAssetMenuExtenderDelegates.Last().GetHandle();
}

void FGitSourceControlModule::ShutdownModule()
{
	// shut down the provider, as this module is going away
	GitSourceControlProvider.Close();

	// unbind provider from editor
    IModularFeatures::Get().UnregisterModularFeature( NAME_SourceControl, &GitSourceControlProvider );


	// Unregister ContentBrowserDelegate Handles
    FContentBrowserModule & ContentBrowserModule = FModuleManager::Get().GetModuleChecked< FContentBrowserModule >( NAME_ContentBrowser );
#if ENGINE_MAJOR_VERSION >= 5
	ContentBrowserModule.GetOnFilterChanged().Remove( CbdHandle_OnFilterChanged );
	ContentBrowserModule.GetOnSearchBoxChanged().Remove( CbdHandle_OnSearchBoxChanged );
	ContentBrowserModule.GetOnAssetSelectionChanged().Remove( CbdHandle_OnAssetSelectionChanged );
	ContentBrowserModule.GetOnAssetPathChanged().Remove( CbdHandle_OnAssetPathChanged );
#endif
	
	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBAssetMenuExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	CBAssetMenuExtenderDelegates.RemoveAll([ &ExtenderDelegateHandle = CbdHandle_OnExtendAssetSelectionMenu ]( const FContentBrowserMenuExtender_SelectedAssets& Delegate ) {
		return Delegate.GetHandle() == ExtenderDelegateHandle;
	});
}

void FGitSourceControlModule::SaveSettings()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}

	GitSourceControlSettings.SaveSettings();
}

void FGitSourceControlModule::SetLastErrors(const TArray<FText>& InErrors)
{
	FGitSourceControlModule* Module = FModuleManager::GetModulePtr<FGitSourceControlModule>("GitSourceControl");
	if (Module)
	{
		Module->GetProvider().SetLastErrors(InErrors);
	}
}

TSharedRef<FExtender> FGitSourceControlModule::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender(new FExtender());
	
	Extender->AddMenuExtension(
		"AssetSourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw( this, &FGitSourceControlModule::CreateGitContentBrowserAssetMenu, SelectedAssets )
	);

	return Extender;
}

void FGitSourceControlModule::CreateGitContentBrowserAssetMenu(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets)
{
	if (!FGitSourceControlModule::Get().GetProvider().GetStatusBranchNames().Num())
	{
		return;
	}
	
	const TArray<FString>& StatusBranchNames = FGitSourceControlModule::Get().GetProvider().GetStatusBranchNames();
	const FString& BranchName = StatusBranchNames[0];
	MenuBuilder.AddMenuEntry(
		FText::Format(LOCTEXT("StatusBranchDiff", "Diff against status branch"), FText::FromString(BranchName)),
		FText::Format(LOCTEXT("StatusBranchDiffDesc", "Compare this asset to the latest status branch version"), FText::FromString(BranchName)),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Diff"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Diff"),
#endif
		FUIAction(FExecuteAction::CreateRaw( this, &FGitSourceControlModule::DiffAssetAgainstGitOriginBranch, SelectedAssets, BranchName ))
	);
}

void FGitSourceControlModule::DiffAssetAgainstGitOriginBranch(const TArray<FAssetData> SelectedAssets, FString BranchName) const
{
	for (int32 AssetIdx = 0; AssetIdx < SelectedAssets.Num(); AssetIdx++)
	{
		// Get the actual asset (will load it)
		const FAssetData& AssetData = SelectedAssets[AssetIdx];

		if (UObject* CurrentObject = AssetData.GetAsset())
		{
			const FString PackagePath = AssetData.PackageName.ToString();
			const FString PackageName = AssetData.AssetName.ToString();
			DiffAgainstOriginBranch(CurrentObject, PackagePath, PackageName, BranchName);
		}
	}
}

void FGitSourceControlModule::DiffAgainstOriginBranch( UObject * InObject, const FString & InPackagePath, const FString & InPackageName, const FString & BranchName ) const
{
	check(InObject);

	const FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
	const FString& PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	const FString& PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();

	const FAssetToolsModule& AssetToolsModule = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");

	// Get the SCC state
	const FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(InPackagePath), EStateCacheUsage::Use);

	// If we have an asset and its in SCC..
	if (SourceControlState.IsValid() && InObject != nullptr && SourceControlState->IsSourceControlled())
	{
		// Get the file name of package
		FString RelativeFileName;
#if ENGINE_MAJOR_VERSION >= 5
		if (FPackageName::DoesPackageExist(InPackagePath, &RelativeFileName))
#else
		if (FPackageName::DoesPackageExist(InPackagePath, nullptr, &RelativeFileName))
#endif
		{
			// if(SourceControlState->GetHistorySize() > 0)
			{
				TArray<FString> Errors;
				const auto& Revision = GitSourceControlUtils::GetOriginRevisionOnBranch(PathToGitBinary, PathToRepositoryRoot, RelativeFileName, Errors, BranchName);

				check(Revision.IsValid());

				FString TempFileName;
				if (Revision->Get(TempFileName))
				{
					// Try and load that package
					UPackage* TempPackage = LoadPackage(nullptr, *TempFileName, LOAD_ForDiff | LOAD_DisableCompileOnLoad);
					if (TempPackage != nullptr)
					{
						// Grab the old asset from that old package
						UObject* OldObject = FindObject<UObject>(TempPackage, *InPackageName);
						if (OldObject != nullptr)
						{
							/* Set the revision information*/
							FRevisionInfo OldRevision;
							OldRevision.Changelist = Revision->GetCheckInIdentifier();
							OldRevision.Date = Revision->GetDate();
							OldRevision.Revision = Revision->GetRevision();

							FRevisionInfo NewRevision;
							NewRevision.Revision = TEXT("");

							AssetToolsModule.Get().DiffAssets(OldObject, InObject, OldRevision, NewRevision);
						}
					}
				}
			}
		}
	}
}

IMPLEMENT_MODULE( FGitSourceControlModule, GitSourceControl );

#undef LOCTEXT_NAMESPACE
