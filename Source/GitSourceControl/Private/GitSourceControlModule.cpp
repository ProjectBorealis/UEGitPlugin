// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "GitSourceControlOperations.h"

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
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	CbdHandle_OnFilterChanged = ContentBrowserModule.GetOnFilterChanged().AddLambda( [this]( const FARFilter&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
	CbdHandle_OnSearchBoxChanged = ContentBrowserModule.GetOnSearchBoxChanged().AddLambda( [this]( const FText&, bool ){ GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetSelectionChanged = ContentBrowserModule.GetOnAssetSelectionChanged().AddLambda( [this]( const TArray<FAssetData>&, bool ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 1; } );
	CbdHandle_OnAssetPathChanged = ContentBrowserModule.GetOnAssetPathChanged().AddLambda( [this]( const FString& ) { GitSourceControlProvider.TicksUntilNextForcedUpdate = 2; } );
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
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.GetOnFilterChanged().Remove( CbdHandle_OnFilterChanged );
	ContentBrowserModule.GetOnSearchBoxChanged().Remove( CbdHandle_OnSearchBoxChanged );
	ContentBrowserModule.GetOnAssetSelectionChanged().Remove( CbdHandle_OnAssetSelectionChanged );
	ContentBrowserModule.GetOnAssetPathChanged().Remove( CbdHandle_OnAssetPathChanged );
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

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);

#undef LOCTEXT_NAMESPACE
