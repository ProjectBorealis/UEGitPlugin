// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlState.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "Textures/SlateIcon.h"
#if ENGINE_MINOR_VERSION >= 2
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif
#endif

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
	// short hash must be >= 7 characters to have a reasonable probability of finding the correct revision
	if (!ensure(InRevision.Len() < 7))
	{
		return nullptr;
	}

	for (const auto& Revision : History)
	{
		// support for short hashes
		const int32 Len = FMath::Min(Revision->FileHash.Len(), InRevision.Len());

		if (Revision->FileHash.Left(Len) == InRevision.Left(Len))
		{
			return Revision;
		}
	}

	return nullptr;
}

#if ENGINE_MAJOR_VERSION < 5 || ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3
TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetBaseRevForMerge() const
{
	for(const auto& Revision : History)
	{
		// look for the the SHA1 id of the file, not the commit id (revision)
		if (Revision->FileHash == PendingMergeBaseFileHash)
		{
			return Revision;
		}
	}

	return nullptr;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetCurrentRevision() const
{
	return nullptr;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
ISourceControlState::FResolveInfo FGitSourceControlState::GetResolveInfo() const
{
	return PendingResolveInfo;
}
#endif

// @todo add Slate icons for git specific states (NotAtHead vs Conflicted...)

#if ENGINE_MAJOR_VERSION < 5
#define GET_ICON_RETURN( NAME ) FName( "ContentBrowser.SCC_" #NAME )
FName FGitSourceControlState::GetIconName() const
{
#else
#if ENGINE_MINOR_VERSION >= 2
#define GET_ICON_RETURN( NAME ) FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl." #NAME )
#else
#define GET_ICON_RETURN( NAME ) FSlateIcon(FAppStyle::GetAppStyleSetName(), "Perforce." #NAME )
#endif
FSlateIcon FGitSourceControlState::GetIcon() const
{
#endif
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return GET_ICON_RETURN(NotAtHeadRevision);
	case EGitState::LockedOther:
		return GET_ICON_RETURN(CheckedOutByOtherUser);
	case EGitState::NotLatest:
		return GET_ICON_RETURN(ModifiedOtherBranch);
	case EGitState::Unmerged:
		return GET_ICON_RETURN(Branched);
	case EGitState::Added:
		return GET_ICON_RETURN(OpenForAdd);
	case EGitState::Untracked:
		return GET_ICON_RETURN(NotInDepot);
	case EGitState::Deleted:
		return GET_ICON_RETURN(MarkedForDelete);
	case EGitState::Modified:
	case EGitState::CheckedOut:
		return GET_ICON_RETURN(CheckedOut);
	case EGitState::Ignored:
		return GET_ICON_RETURN(NotInDepot);
	default:
#if ENGINE_MAJOR_VERSION < 5
	  return NAME_None;
#else
	  return FSlateIcon();
#endif
	}
}

#if ENGINE_MAJOR_VERSION < 5
FName FGitSourceControlState::GetSmallIconName() const
{
	switch (GetGitState()) {
	case EGitState::NotAtHead:
	  return FName("ContentBrowser.SCC_NotAtHeadRevision_Small");
	case EGitState::LockedOther:
	  return FName("ContentBrowser.SCC_CheckedOutByOtherUser_Small");
	case EGitState::NotLatest:
	  return FName("ContentBrowser.SCC_ModifiedOtherBranch_Small");
	case EGitState::Unmerged:
	  return FName("ContentBrowser.SCC_Branched_Small");
	case EGitState::Added:
	  return FName("ContentBrowser.SCC_OpenForAdd_Small");
	case EGitState::Untracked:
	  return FName("ContentBrowser.SCC_NotInDepot_Small");
	case EGitState::Deleted:
	  return FName("ContentBrowser.SCC_MarkedForDelete_Small");
	case EGitState::Modified:
        case EGitState::CheckedOut:
                return FName("ContentBrowser.SCC_CheckedOut_Small");
	case EGitState::Ignored:
	  return FName("ContentBrowser.SCC_NotInDepot_Small");
	default:
	  return NAME_None;
	}
}
#endif

FText FGitSourceControlState::GetDisplayName() const
{
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return LOCTEXT("NotCurrent", "Not current");
	case EGitState::LockedOther:
		return FText::Format(LOCTEXT("CheckedOutOther", "Checked out by: {0}"), FText::FromString(State.LockUser));
	case EGitState::NotLatest:
		return FText::Format(LOCTEXT("ModifiedOtherBranch", "Modified in branch: {0}"), FText::FromString(State.HeadBranch));
	case EGitState::Unmerged:
		return LOCTEXT("Conflicted", "Conflicted");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd", "Opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled", "Not Under Revision Control");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete", "Marked for delete");
	case EGitState::Modified:
	case EGitState::CheckedOut:
		return LOCTEXT("CheckedOut", "Checked out");
	case EGitState::Ignored:
		return LOCTEXT("Ignore", "Ignore");
	case EGitState::Lockable:
		return LOCTEXT("ReadOnly", "Read only");
	case EGitState::None:
		return LOCTEXT("Unknown", "Unknown");
	default:
		return FText();
	}
}

FText FGitSourceControlState::GetDisplayTooltip() const
{
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return LOCTEXT("NotCurrent_Tooltip", "The file(s) are not at the head revision");
	case EGitState::LockedOther:
		return FText::Format(LOCTEXT("CheckedOutOther_Tooltip", "Checked out by: {0}"), FText::FromString(State.LockUser));
	case EGitState::NotLatest:
		return FText::Format(LOCTEXT("ModifiedOtherBranch_Tooltip", "Modified in branch: {0} CL:{1} ({2})"), FText::FromString(State.HeadBranch), FText::FromString(HeadCommit), FText::FromString(HeadAction));
	case EGitState::Unmerged:
		return LOCTEXT("ContentsConflict_Tooltip", "The contents of the item conflict with updates received from the repository.");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd_Tooltip", "The file(s) are opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled_Tooltip", "Item is not under revision control.");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete_Tooltip", "The file(s) are marked for delete");
	case EGitState::Modified:
	case EGitState::CheckedOut:
		return LOCTEXT("CheckedOut_Tooltip", "The file(s) are checked out");
	case EGitState::Ignored:
		return LOCTEXT("Ignored_Tooltip", "Item is being ignored.");
	case EGitState::Lockable:
		return LOCTEXT("ReadOnly_Tooltip", "The file(s) are marked locally as read-only");
	case EGitState::None:
		return LOCTEXT("Unknown_Tooltip", "Unknown revision control state");
	default:
		return FText();
	}
}

const FString& FGitSourceControlState::GetFilename() const
{
	return LocalFilename;
}

const FDateTime& FGitSourceControlState::GetTimeStamp() const
{
	return TimeStamp;
}

// Deleted and Missing assets cannot appear in the Content Browser, but they do in the Submit files to Revision Control window!
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

	// We can check back in if we're locked.
	if (State.LockState == ELockState::Locked)
	{
		return true;
	}

	// We can check in any file that has been modified, unless someone else locked it.
	if (State.LockState != ELockState::LockedOther && IsModified() && IsSourceControlled())
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
		return IsSourceControlled(); // TODO: try modified instead? might block editing the file with a holding pattern
	}
	else
	{
		// We check for modified here too, because sometimes you don't lock a file but still want to push it. CanCheckout still true, so that you can lock it later...
		return State.LockState == ELockState::Locked || (State.FileState == EFileState::Modified && State.LockState != ELockState::LockedOther);
	}
}

bool FGitSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who != nullptr)
	{
		// The packages dialog uses our lock user regardless if it was locked by other or us.
		// But, if there is no lock user, it shows information about modification in other branches, which is important.
		// So, only show our own lock user if it hasn't been modified in another branch.
		// This is a very, very rare state (maybe impossible), but one that should be displayed properly.
		if (State.LockState == ELockState::LockedOther || (State.LockState == ELockState::Locked && !IsModifiedInOtherBranch()))
		{
			*Who = State.LockUser;
		}
	}
	return State.LockState == ELockState::LockedOther;
}

bool FGitSourceControlState::IsCheckedOutInOtherBranch(const FString& CurrentBranch) const
{
	// You can't check out separately per branch
	return false;
}

bool FGitSourceControlState::IsModifiedInOtherBranch(const FString& CurrentBranch) const
{
	return State.RemoteState == ERemoteState::NotLatest;
}

bool FGitSourceControlState::GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const
{
	if (!IsModifiedInOtherBranch())
	{
		return false;
	}

	HeadBranchOut = State.HeadBranch;
	ActionOut = HeadAction; // TODO: from ERemoteState
	HeadChangeListOut = 0; // TODO: get head commit
	return true;
}

bool FGitSourceControlState::IsCurrent() const
{
	return State.RemoteState != ERemoteState::NotAtHead && State.RemoteState != ERemoteState::NotLatest;
}

bool FGitSourceControlState::IsSourceControlled() const
{
	return State.TreeState != ETreeState::Untracked && State.TreeState != ETreeState::Ignored && State.TreeState != ETreeState::NotInRepo;
}

bool FGitSourceControlState::IsAdded() const
{
	// Added is when a file was untracked and is now added.
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
	// If someone else hasn't checked it out, we can delete revision controlled files.
	return !IsCheckedOutOther() && IsSourceControlled();
}

bool FGitSourceControlState::IsUnknown() const
{
	return State.FileState == EFileState::Unknown && State.TreeState == ETreeState::NotInRepo;
}

bool FGitSourceControlState::IsModified() const
{
	return State.TreeState == ETreeState::Working ||
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
	// Can revert the file state if we modified, even if it was locked by someone else.
	// Useful for when someone locked a file, and you just wanna play around with it locallly, and then revert it.
	return CanCheckIn() || IsModified();
}

EGitState::Type FGitSourceControlState::GetGitState() const
{
	// No matter what, we must pull from remote, even if we have locked or if we have modified.
	switch (State.RemoteState)
	{
	case ERemoteState::NotAtHead:
		return EGitState::NotAtHead;
	default:
		break;
	}

	/** Someone else locked this file across branches. */
	// We cannot push under any circumstance, if someone else has locked.
	if (State.LockState == ELockState::LockedOther)
	{
		return EGitState::LockedOther;
	}

	// We could theoretically push, but we shouldn't.
	if (State.RemoteState == ERemoteState::NotLatest)
	{
		return EGitState::NotLatest;
	}

	switch (State.FileState)
	{
	case EFileState::Unmerged:
		return EGitState::Unmerged;
	case EFileState::Added:
		return EGitState::Added;
	case EFileState::Deleted:
		return EGitState::Deleted;
	case EFileState::Modified:
		return EGitState::Modified;
	default:
		break;
	}

	if (State.TreeState == ETreeState::Untracked)
	{
		return EGitState::Untracked;
	}

	if (State.LockState == ELockState::Locked)
	{
		return EGitState::CheckedOut;
	}

	if (IsSourceControlled())
	{
		if (CanCheckout())
		{
			return EGitState::Lockable;
		}
		return EGitState::Unmodified;
	}

	return EGitState::None;
}

#undef LOCTEXT_NAMESPACE
