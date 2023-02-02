// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "AssetToolsModule.h"
#include "EditorStyleSet.h"
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
#include "Framework/MultiBox/MultiBoxBuilder.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "ContentBrowser/Public/ContentBrowserModule.h"
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

TArray<FString> FGitSourceControlModule::EmptyStringArray;

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

	// load our settings
	GitSourceControlSettings.LoadSettings();

	// Bind our source control provider to the editor
	IModularFeatures::Get().RegisterModularFeature( "SourceControl", &GitSourceControlProvider );

#if ENGINE_MAJOR_VERSION >= 5
	// Register ContentBrowserDelegate Handles for UE5 EA
	// At the time of writing this UE5 is in Early Access and has no support for source control yet. So instead we hook into the contentbrowser..
	// .. and force a state update on the next tick for source control. Usually the contentbrowser assets will request this themselves, but that's not working
	// Values here are 1 or 2 based on whether the change can be done immediately or needs to be delayed as unreal needs to work through its internal delegates first
	// >> Technically you wouldn't need to use `GetOnAssetSelectionChanged` -- but it's there as a safety mechanism. States aren't forceupdated for the first path that loads
	// >> Making sure we force an update on selection change that acts like a just in case other measures fail
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	CbdHandle_OnFilterChanged = ContentBrowserModule.GetOnFilterChanged().AddLambda( [this]( const FARFilter&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
	CbdHandle_OnSearchBoxChanged = ContentBrowserModule.GetOnSearchBoxChanged().AddLambda( [this]( const FText&, bool ){ GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetSelectionChanged = ContentBrowserModule.GetOnAssetSelectionChanged().AddLambda( [this]( const TArray<FAssetData>&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetPathChanged = ContentBrowserModule.GetOnAssetPathChanged().AddLambda( [this]( const FString& ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );

    auto  & extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
    extenders.Add( FContentBrowserMenuExtender_SelectedAssets::CreateRaw( this, &FGitSourceControlModule::OnExtendContentBrowserAssetSelectionMenu ) );
    ContentBrowserAssetExtenderDelegateHandle = extenders.Last().GetHandle();
#endif
}

void FGitSourceControlModule::ShutdownModule()
{
	// shut down the provider, as this module is going away
	GitSourceControlProvider.Close();

	// unbind provider from editor
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &GitSourceControlProvider);

#if ENGINE_MAJOR_VERSION >= 5
	// Unregister ContentBrowserDelegate Handles
    FContentBrowserModule & ContentBrowserModule = FModuleManager::Get().LoadModuleChecked< FContentBrowserModule >( "ContentBrowser" );
	ContentBrowserModule.GetOnFilterChanged().Remove( CbdHandle_OnFilterChanged );
	ContentBrowserModule.GetOnSearchBoxChanged().Remove( CbdHandle_OnSearchBoxChanged );
	ContentBrowserModule.GetOnAssetSelectionChanged().Remove( CbdHandle_OnAssetSelectionChanged );
	ContentBrowserModule.GetOnAssetPathChanged().Remove( CbdHandle_OnAssetPathChanged );

	auto & extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
    extenders.RemoveAll( [ &extender_delegate = ContentBrowserAssetExtenderDelegateHandle ]( const FContentBrowserMenuExtender_SelectedAssets & delegate ) {
        return delegate.GetHandle() == extender_delegate;
    } );
#endif
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

TSharedRef<FExtender> FGitSourceControlModule::OnExtendContentBrowserAssetSelectionMenu( const TArray<FAssetData> & selected_assets )
{
    TSharedRef< FExtender > extender( new FExtender() );

    extender->AddMenuExtension(
        "AssetSourceControlActions",
        EExtensionHook::After,
        nullptr,
        FMenuExtensionDelegate::CreateRaw( this, &FGitSourceControlModule::CreateGitContentBrowserAssetMenu, selected_assets ) );

    return extender;
}

void FGitSourceControlModule::CreateGitContentBrowserAssetMenu( FMenuBuilder & menu_builder, const TArray<FAssetData> selected_assets )
{
    menu_builder.AddMenuEntry(
        LOCTEXT( "GitPlugin", "Diff against origin/develop" ),
        LOCTEXT( "GitPlugin", "Diff that asset against the version on origin/develop." ),
        FSlateIcon( FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Diff" ),
        FUIAction( FExecuteAction::CreateRaw( this, &FGitSourceControlModule::DiffAssetAgainstGitOriginDevelop, selected_assets ) ) );
}

void FGitSourceControlModule::DiffAssetAgainstGitOriginDevelop( const TArray<FAssetData> selected_assets ) const
{
    for ( int32 AssetIdx = 0; AssetIdx < selected_assets.Num(); AssetIdx++ )
    {
        // Get the actual asset (will load it)
        const FAssetData & AssetData = selected_assets[ AssetIdx ];

        if ( UObject * CurrentObject = AssetData.GetAsset() )
        {
            const FString PackagePath = AssetData.PackageName.ToString();
            const FString PackageName = AssetData.AssetName.ToString();
            DiffAgainstOriginDevelop( CurrentObject, PackagePath, PackageName );
        }
    }
}

void FGitSourceControlModule::DiffAgainstOriginDevelop( UObject * InObject, const FString & InPackagePath, const FString & InPackageName ) const
{
    check( InObject );

    const FGitSourceControlModule & GitSourceControl = FModuleManager::GetModuleChecked< FGitSourceControlModule >( "GitSourceControl" );
    const auto PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
    const auto PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();

    ISourceControlProvider & SourceControlProvider = ISourceControlModule::Get().GetProvider();

    const FAssetToolsModule & AssetToolsModule = FModuleManager::GetModuleChecked< FAssetToolsModule >( "AssetTools" );

    // Get the SCC state
    const FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState( SourceControlHelpers::PackageFilename( InPackagePath ), EStateCacheUsage::Use );

    // If we have an asset and its in SCC..
    if ( SourceControlState.IsValid() && InObject != nullptr && SourceControlState->IsSourceControlled() )
    {
        // Get the file name of package
        FString RelativeFileName;
        if ( FPackageName::DoesPackageExist( InPackagePath, &RelativeFileName ) )
        {
            //if(SourceControlState->GetHistorySize() > 0)
            {
                TArray< FString > Errors;
                const auto Revision = GitSourceControlUtils::GetOriginDevelopRevision( PathToGitBinary, PathToRepositoryRoot, RelativeFileName, Errors );

                check( Revision.IsValid() );

                FString TempFileName;
                if ( Revision->Get( TempFileName ) )
                {
                    // Try and load that package
                    UPackage * TempPackage = LoadPackage( nullptr, *TempFileName, LOAD_ForDiff | LOAD_DisableCompileOnLoad );
                    if ( TempPackage != nullptr )
                    {
                        // Grab the old asset from that old package
                        UObject * OldObject = FindObject< UObject >( TempPackage, *InPackageName );
                        if ( OldObject != nullptr )
                        {
                            /* Set the revision information*/
                            FRevisionInfo OldRevision;
                            OldRevision.Changelist = Revision->GetCheckInIdentifier();
                            OldRevision.Date = Revision->GetDate();
                            OldRevision.Revision = Revision->GetRevision();

                            FRevisionInfo NewRevision;
                            NewRevision.Revision = TEXT( "" );

                            AssetToolsModule.Get().DiffAssets( OldObject, InObject, OldRevision, NewRevision );
                        }
                    }
                }
            }
        }
    }
}

IMPLEMENT_MODULE( FGitSourceControlModule, GitSourceControl );

#undef LOCTEXT_NAMESPACE
