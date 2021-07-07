// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlState.h"

#define LOCTEXT_NAMESPACE "GitSourceControl.State"

int32 FGitSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetHistoryItem( int32 HistoryIndex ) const
{
	check(History.IsValidIndex(HistoryIndex));
	return History[HistoryIndex];
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (auto Iter(History.CreateConstIterator()); Iter; Iter++)
	{
		if ((*Iter)->GetRevisionNumber() == RevisionNumber)
		{
			return *Iter;
		}
	}

	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
		{
			return Revision;
		}
	}

	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetBaseRevForMerge() const
{
	for(const auto& Revision : History)
	{
		// look for the the SHA1 id of the file, not the commit id (revision)
		if(Revision->FileHash == PendingMergeBaseFileHash)
		{
			return Revision;
		}
	}

	return nullptr;
}

// @todo add Slate icons for git specific states (NotAtHead vs Conflicted...)
FName FGitSourceControlState::GetIconName() const
{
	if(LockState == ELockState::Locked)
	{
		return FName("Subversion.CheckedOut");
	}
	else if(LockState == ELockState::LockedOther)
	{
		return FName("Subversion.CheckedOutByOtherUser");
	}
	else if (!IsCurrent())
	{
		return FName("Subversion.NotAtHeadRevision");
	}

	switch(State)
	{
	case EWorkingCopyState::Modified:
		if(bUsingGitLfsLocking)
		{
			return FName("Subversion.NotInDepot");
		}
		else
		{
			return FName("Subversion.CheckedOut");
		}
	case EWorkingCopyState::Added:
		return FName("Subversion.OpenForAdd");
	case EWorkingCopyState::Renamed:
	case EWorkingCopyState::Copied:
		return FName("Subversion.Branched");
	case EWorkingCopyState::Deleted: // Deleted & Missing files does not show in Content Browser
	case EWorkingCopyState::Missing:
		return FName("Subversion.MarkedForDelete");
	case EWorkingCopyState::Conflicted:
		return FName("Subversion.ModifiedOtherBranch");
	case EWorkingCopyState::NotControlled:
		return FName("Subversion.NotInDepot");
	case EWorkingCopyState::Unknown:
	case EWorkingCopyState::Unchanged: // Unchanged is the same as "Pristine" (not checked out) for Perforce, ie no icon
	case EWorkingCopyState::Ignored:
	default:
		return NAME_None;
	}

	return NAME_None;
}

FName FGitSourceControlState::GetSmallIconName() const
{
	if(LockState == ELockState::Locked)
	{
		return FName("Subversion.CheckedOut_Small");
	}
	else if(LockState == ELockState::LockedOther)
	{
		return FName("Subversion.CheckedOutByOtherUser_Small");
	}
	else if (!IsCurrent())
	{
		return FName("Subversion.NotAtHeadRevision_Small");
	}

	switch(State)
	{
	case EWorkingCopyState::Modified:
		if(bUsingGitLfsLocking)
		{
			return FName("Subversion.NotInDepot_Small");
		}
		else
		{
			return FName("Subversion.CheckedOut_Small");
		}
	case EWorkingCopyState::Added:
		return FName("Subversion.OpenForAdd_Small");
	case EWorkingCopyState::Renamed:
	case EWorkingCopyState::Copied:
		return FName("Subversion.Branched_Small");
	case EWorkingCopyState::Deleted: // Deleted & Missing files can appear in the Submit to Source Control window
	case EWorkingCopyState::Missing:
		return FName("Subversion.MarkedForDelete_Small");
	case EWorkingCopyState::Conflicted:
		return FName("Subversion.ModifiedOtherBranch_Small");
	case EWorkingCopyState::NotControlled:
		return FName("Subversion.NotInDepot_Small");
	case EWorkingCopyState::Unknown:
	case EWorkingCopyState::Unchanged: // Unchanged is the same as "Pristine" (not checked out) for Perforce, ie no icon
	case EWorkingCopyState::Ignored:
	default:
		return NAME_None;
	}

	return NAME_None;
}

FText FGitSourceControlState::GetDisplayName() const
{
	if(LockState == ELockState::Locked)
	{
		return LOCTEXT("Locked", "Locked For Editing");
	}
	else if(LockState == ELockState::LockedOther)
	{
		return FText::Format( LOCTEXT("LockedOther", "Locked by "), FText::FromString(LockUser) );
	}
	else if (!IsCurrent())
	{
		return LOCTEXT("NotCurrent", "Not current");
	}

	switch(State)
	{
	case EWorkingCopyState::Unknown:
		return LOCTEXT("Unknown", "Unknown");
	case EWorkingCopyState::Unchanged:
		return LOCTEXT("Unchanged", "Unchanged");
	case EWorkingCopyState::Added:
		return LOCTEXT("Added", "Added");
	case EWorkingCopyState::Deleted:
		return LOCTEXT("Deleted", "Deleted");
	case EWorkingCopyState::Modified:
		return LOCTEXT("Modified", "Modified");
	case EWorkingCopyState::Renamed:
		return LOCTEXT("Renamed", "Renamed");
	case EWorkingCopyState::Copied:
		return LOCTEXT("Copied", "Copied");
	case EWorkingCopyState::Conflicted:
		return LOCTEXT("ContentsConflict", "Contents Conflict");
	case EWorkingCopyState::Ignored:
		return LOCTEXT("Ignored", "Ignored");
	case EWorkingCopyState::NotControlled:
		return LOCTEXT("NotControlled", "Not Under Source Control");
	case EWorkingCopyState::Missing:
		return LOCTEXT("Missing", "Missing");
	}

	return FText();
}

FText FGitSourceControlState::GetDisplayTooltip() const
{
	if(LockState == ELockState::Locked)
	{
		return LOCTEXT("Locked_Tooltip", "Locked for editing by current user");
	}
	else if(LockState == ELockState::LockedOther)
	{
		return FText::Format( LOCTEXT("LockedOther_Tooltip", "Locked for editing by: {0}"), FText::FromString(LockUser) );
	}
	else if (!IsCurrent())
	{
		return LOCTEXT("NotCurrent_Tooltip", "The file(s) are not at the head revision");
	}

	switch(State)
	{
	case EWorkingCopyState::Unknown:
		return LOCTEXT("Unknown_Tooltip", "Unknown source control state");
	case EWorkingCopyState::Unchanged:
		return LOCTEXT("Pristine_Tooltip", "There are no modifications");
	case EWorkingCopyState::Added:
		return LOCTEXT("Added_Tooltip", "Item is scheduled for addition");
	case EWorkingCopyState::Deleted:
		return LOCTEXT("Deleted_Tooltip", "Item is scheduled for deletion");
	case EWorkingCopyState::Modified:
		return LOCTEXT("Modified_Tooltip", "Item has been modified");
	case EWorkingCopyState::Renamed:
		return LOCTEXT("Renamed_Tooltip", "Item has been renamed");
	case EWorkingCopyState::Copied:
		return LOCTEXT("Copied_Tooltip", "Item has been copied");
	case EWorkingCopyState::Conflicted:
		return LOCTEXT("ContentsConflict_Tooltip", "The contents of the item conflict with updates received from the repository.");
	case EWorkingCopyState::Ignored:
		return LOCTEXT("Ignored_Tooltip", "Item is being ignored.");
	case EWorkingCopyState::NotControlled:
		return LOCTEXT("NotControlled_Tooltip", "Item is not under version control.");
	case EWorkingCopyState::Missing:
		return LOCTEXT("Missing_Tooltip", "Item is missing (e.g., you moved or deleted it without using Git). This also indicates that a directory is incomplete (a checkout or update was interrupted).");
	}

	return FText();
}

const FString& FGitSourceControlState::GetFilename() const
{
	return LocalFilename;
}

const FDateTime& FGitSourceControlState::GetTimeStamp() const
{
	return TimeStamp;
}

// Deleted and Missing assets cannot appear in the Content Browser, but the do in the Submit files to Source Control window!
bool FGitSourceControlState::CanCheckIn() const
{
	// We can check in if this is new content
	if (IsAdded())
	{
		return true;
	}

	// Cannot check back in if conflicted or not current 
	if (!IsCurrent() || IsConflicted())
	{
		return false;
	}

	// We can check back in if we checked out.
	if (IsCheckedOut())
	{
		return true;
	}

	// We can check in any file that's not lockable but has been modified.
	if (State.LockState == ELockState::Unlockable && IsModified())
	{
		return true;
	}

	return false;
}

bool FGitSourceControlState::CanCheckout() const
{
	if (State.LockState == ELockState::Unlockable)
	{
		// Everything is already available for check in (checked out).
		return false;
	}
	else
	{
		// We don't want to allow checkout if the file is out-of-date, as modifying an out-of-date binary file will most likely result in a merge conflict
		return State.LockState == ELockState::NotLocked && IsCurrent();
	}
}

bool FGitSourceControlState::IsCheckedOut() const
{
	if (State.LockState == ELockState::Unlockable)
	{
		return IsSourceControlled();
	}
	{
		return State.LockState == ELockState::Locked;
	}
}

bool FGitSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who != NULL)
	{
		*Who = LockUser;
	}
	return State.LockState == ELockState::LockedOther;
}

bool FGitSourceControlState::IsCurrent() const
{
	return State.RemoteState != ERemoteState::NotAtHead;
}

bool FGitSourceControlState::IsSourceControlled() const
{
	return State.TreeState != ETreeState::Untracked || State.TreeState != ETreeState::Ignored && State.TreeState != ETreeState::NotInRepo;
}

bool FGitSourceControlState::IsAdded() const
{
	// Untracked files are always eventually added. Added is when a file was untracked and was already added.
	return State.FileState == EFileState::Added;
}

bool FGitSourceControlState::IsDeleted() const
{
	return State.FileState == EFileState::Deleted;
}

bool FGitSourceControlState::IsIgnored() const
{
	return State.TreeState == ETreeState::Ignored;
}

bool FGitSourceControlState::CanEdit() const
{
	// Perforce does not care about it being current
	return IsCheckedOut() || IsAdded();
}

bool FGitSourceControlState::CanDelete() const
{
	// Perforce enforces that a deleted file must be current.
	if (!IsCurrent())
	{
		return false;
	}
	// If someone else hasn't checked it out, we can delete source controlled files.
	return !IsCheckedOutOther() && IsSourceControlled();
}

bool FGitSourceControlState::IsUnknown() const
{
	return State.FileState == EFileState::Unknown;
}

bool FGitSourceControlState::IsModified() const
{
	return State.TreeState == ETreeState::Working ||
		State.TreeState == ETreeState::Untracked ||
		State.TreeState == ETreeState::Staged;
}


bool FGitSourceControlState::CanAdd() const
{
	return State.TreeState == ETreeState::Untracked;
}

bool FGitSourceControlState::IsConflicted() const
{
	return State.FileState == EFileState::Unmerged;
}

bool FGitSourceControlState::CanRevert() const
{
	return IsConflicted() || CanCheckIn();
}

#undef LOCTEXT_NAMESPACE
