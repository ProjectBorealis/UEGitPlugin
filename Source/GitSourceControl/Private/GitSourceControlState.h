// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlRevision.h"
#include "Runtime/Launch/Resources/Version.h"

/** A consolidation of state priorities. */
namespace EGitState
{
	enum Type
	{
		Unset,
		NotAtHead,
#if 0
		AddedAtHead,
		DeletedAtHead,
#endif
		LockedOther,
		NotLatest,
		/** Unmerged state (modified, but conflicts) */
		Unmerged,
		Added,
		Deleted,
		Modified,
		/** Not modified, but locked explicitly. */
		CheckedOut,
		Untracked,
		Lockable,
		Unmodified,
		Ignored,
		/** Whatever else. */
		None,
	};
}

/** Corresponds to diff file states. */
namespace EFileState
{
	enum Type
	{
		Unset,
		Unknown,
		Added,
		Copied,
		Deleted,
		Modified,
		Renamed,
		Missing,
		Unmerged,
	};
}

/** Where in the world is this file? */
namespace ETreeState
{
	enum Type
	{
		Unset,
		/** This file is synced to commit */
		Unmodified,
		/** This file is modified, but not in staging tree */
		Working,
		/** This file is in staging tree (git add) */
		Staged,
		/** This file is not tracked in the repo yet */
		Untracked,
		/** This file is ignored by the repo */
		Ignored,
		/** This file is outside the repo folder */
		NotInRepo,
	};
}

/** LFS locks status of this file */
namespace ELockState
{
	enum Type
	{
		Unset,
		Unknown,
		Unlockable,
		NotLocked,
		Locked,
		LockedOther,
	};
}

/** What is this file doing at HEAD? */
namespace ERemoteState
{
	enum Type
	{
		Unset,
		/** Up to date */
		UpToDate,
		/** Local version is behind remote */
		NotAtHead,
#if 0
		// TODO: handle these
		/** Remote file does not exist on local */
		AddedAtHead,
		/** Local was deleted on remote */
		DeletedAtHead,
#endif
		/** Not at the latest revision amongst the tracked branches */
		NotLatest,
	};
}

/** Combined state, for updating cache in a map. */
struct FGitState
{
	EFileState::Type FileState = EFileState::Unknown;
	ETreeState::Type TreeState = ETreeState::NotInRepo;
	ELockState::Type LockState = ELockState::Unknown;
	/** Name of user who has locked the file */
	FString LockUser;
	ERemoteState::Type RemoteState = ERemoteState::UpToDate;
	/** The branch with the latest commit for this file */
	FString HeadBranch;
};

class FGitSourceControlState : public ISourceControlState
{
public:
	FGitSourceControlState(const FString &InLocalFilename)
		: LocalFilename(InLocalFilename)
		, TimeStamp(0)
		, HeadAction(TEXT("Changed"))
		, HeadCommit(TEXT("Unknown"))
	{
	}

	/** ISourceControlState interface */
	virtual int32 GetHistorySize() const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override;
#if ENGINE_MAJOR_VERSION < 5 || ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetBaseRevForMerge() const override;
#else
	virtual FResolveInfo GetResolveInfo() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override;
#endif
#if ENGINE_MAJOR_VERSION >= 5
	virtual FSlateIcon GetIcon() const override;
#else
	virtual FName GetIconName() const override;
	virtual FName GetSmallIconName() const override;
#endif
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual const FString& GetFilename() const override;
	virtual const FDateTime& GetTimeStamp() const override;
	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = NULL) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override;
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override;
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return IsModifiedInOtherBranch(CurrentBranch); }
	virtual TArray<FString> GetCheckedOutBranches() const override { return TArray<FString>(); }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override;
	virtual bool IsCurrent() const override;
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override;
	virtual bool IsDeleted() const override;
	virtual bool IsIgnored() const override;
	virtual bool CanEdit() const override;
	virtual bool IsUnknown() const override;
	virtual bool IsModified() const override;
	virtual bool CanAdd() const override;
	virtual bool CanDelete() const override;
	virtual bool IsConflicted() const override;
	virtual bool CanRevert() const override;

private:
	EGitState::Type GetGitState() const;

public:
	/** History of the item, if any */
	TGitSourceControlHistory History;

	/** Filename on disk */
	FString LocalFilename;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	/** Pending rev info with which a file must be resolved, invalid if no resolve pending */
	FResolveInfo PendingResolveInfo;

	UE_DEPRECATED(5.3, "Use PendingResolveInfo.BaseRevision instead")
#endif
	/** File Id with which our local revision diverged from the remote revision */
	FString PendingMergeBaseFileHash;

	/** Status of the file */
	FGitState State;

	/** The timestamp of the last update */
	FDateTime TimeStamp;

	/** The action within the head branch TODO */
	FString HeadAction;

	/** The last file modification time in the head branch TODO */
	int64 HeadModTime;

	/** The change list the last modification TODO */
	FString HeadCommit;
};
