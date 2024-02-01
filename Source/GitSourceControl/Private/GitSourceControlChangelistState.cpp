#include "GitSourceControlChangelistState.h"

#define LOCTEXT_NAMESPACE "GitSourceControl.ChangelistState"

FName FGitSourceControlChangelistState::GetIconName() const
{
	// Mimic P4V colors, returning the red icon if there are active file(s), the blue if the changelist is empty or all the files are shelved.
	return FName("SourceControl.Changelist");
}

FName FGitSourceControlChangelistState::GetSmallIconName() const
{
	return GetIconName();
}

FText FGitSourceControlChangelistState::GetDisplayText() const
{
	return FText::FromString(Changelist.GetName());
}

FText FGitSourceControlChangelistState::GetDescriptionText() const
{
	return FText::FromString(Description);
}

FText FGitSourceControlChangelistState::GetDisplayTooltip() const
{
	return LOCTEXT("Tooltip", "Tooltip");
}

const FDateTime& FGitSourceControlChangelistState::GetTimeStamp() const
{
	return TimeStamp;
}

const TArray<FSourceControlStateRef>& FGitSourceControlChangelistState::GetFilesStates() const
{
	return Files;
}

const TArray<FSourceControlStateRef>& FGitSourceControlChangelistState::GetShelvedFilesStates() const
{
	return ShelvedFiles;
}

FSourceControlChangelistRef FGitSourceControlChangelistState::GetChangelist() const
{
	FGitSourceControlChangelistRef ChangelistCopy = MakeShareable( new FGitSourceControlChangelist(Changelist));
	return StaticCastSharedRef<ISourceControlChangelist>(ChangelistCopy);
}

#undef LOCTEXT_NAMESPACE