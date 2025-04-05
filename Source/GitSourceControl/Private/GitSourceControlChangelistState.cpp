#include "GitSourceControlChangelistState.h"

#if ENGINE_MAJOR_VERSION == 5
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

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
const TArray<FSourceControlStateRef> FGitSourceControlChangelistState::GetFilesStates() const
#else
const TArray<FSourceControlStateRef>& FGitSourceControlChangelistState::GetFilesStates() const
#endif
{
	return Files;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
int32 FGitSourceControlChangelistState::GetFilesStatesNum() const
{
	return Files.Num();
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
const TArray<FSourceControlStateRef> FGitSourceControlChangelistState::GetShelvedFilesStates() const
#else
const TArray<FSourceControlStateRef>& FGitSourceControlChangelistState::GetShelvedFilesStates() const
#endif
{
	return ShelvedFiles;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
int32 FGitSourceControlChangelistState::GetShelvedFilesStatesNum() const
{
	return ShelvedFiles.Num();
}
#endif

FSourceControlChangelistRef FGitSourceControlChangelistState::GetChangelist() const
{
	FGitSourceControlChangelistRef ChangelistCopy = MakeShareable( new FGitSourceControlChangelist(Changelist));
	return StaticCastSharedRef<ISourceControlChangelist>(ChangelistCopy);
}

#undef LOCTEXT_NAMESPACE
#endif
