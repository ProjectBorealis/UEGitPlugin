// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"
#include "GitSourceControlRevision.h"

namespace EFileState
{
	enum Type
	{
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

namespace ETreeState
{
	enum Type
	{
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

namespace ERemoteState
{
	enum Type
	{
		/** Not at current branch's HEAD */
		NotAtHead,
		/** Not at the latest revision amongst the tracked branches */
		NotLatest,
		/** We want to branch off and ignore modified state */
		Branched,
	};
}

namespace ELockState
{
	enum Type
	{
		Unknown,
		Unlockable,
		NotLocked,
		Locked,
		LockedOther,
	};
}

struct FGitState
{
	EFileState::Type FileState;
	ETreeState::Type TreeState;
	ERemoteState::Type RemoteState;
	ELockState::Type LockState;
};

class FGitSourceControlState : public ISourceControlState, public TSharedFromThis<FGitSourceControlState, ESPMode::ThreadSafe>
{
public:
	FGitSourceControlState( const FString& InLocalFilename, const bool InUsingLfsLocking)
		: LocalFilename(InLocalFilename)
		, WorkingCopyState(EFileState::Unknown)
		, LockState(ELockState::Unknown)
		, bNewerVersionOnServer(false)
		, TimeStamp(0)
	{
		bUsingGitLfsLocking = InUsingLfsLocking && !InLocalFilename.Contains(TEXT(".ini")) && !InLocalFilename.Contains(TEXT(".uproject"));
	}

	/** ISourceControlState interface */
	virtual int32 GetHistorySize() const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetBaseRevForMerge() const override;
	virtual FName GetIconName() const override;
	virtual FName GetSmallIconName() const override;
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
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return IsCheckedOutInOtherBranch(CurrentBranch) || IsModifiedInOtherBranch(CurrentBranch); }
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

public:
	/** History of the item, if any */
	TGitSourceControlHistory History;

	/** Filename on disk */
	FString LocalFilename;

	/** File Id with which our local revision diverged from the remote revision */
	FString PendingMergeBaseFileHash;

	FGitState State;

	/** Name of user who has locked the file */
	FString LockUser;

	/** The timestamp of the last update */
	FDateTime TimeStamp;
};
