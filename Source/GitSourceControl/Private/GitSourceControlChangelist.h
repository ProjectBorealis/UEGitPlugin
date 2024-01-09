#pragma once
#include "ISourceControlChangelist.h"

class FGitSourceControlChangelist : public ISourceControlChangelist
{
public:
	FGitSourceControlChangelist() = default;

	explicit FGitSourceControlChangelist(FString&& InChangelistName, const bool bInInitialized = false)
		: ChangelistName(MoveTemp(InChangelistName))
		  , bInitialized(bInInitialized)
	{
	}

	virtual bool CanDelete() const override
	{
		return false;
	}

	bool operator==(const FGitSourceControlChangelist& InOther) const
	{
		return ChangelistName == InOther.ChangelistName;
	}

	bool operator!=(const FGitSourceControlChangelist& InOther) const
	{
		return ChangelistName != InOther.ChangelistName;
	}

	virtual bool IsDefault() const override
	{
		return ChangelistName == WorkingChangelist.ChangelistName;
	}

	void SetInitialized()
	{
		bInitialized = true;
	}

	bool IsInitialized() const
	{
		return bInitialized;
	}

	void Reset()
	{
		ChangelistName.Reset();
		bInitialized = false;
	}

	friend FORCEINLINE uint32 GetTypeHash(const FGitSourceControlChangelist& InGitChangelist)
	{
		return GetTypeHash(InGitChangelist.ChangelistName);
	}

	FString GetName() const
	{
		return ChangelistName;
	}

	virtual FString GetIdentifier() const override
	{
		return ChangelistName;
	}

public:
	static const FGitSourceControlChangelist WorkingChangelist;
	static const FGitSourceControlChangelist StagedChangelist;

private:
	FString ChangelistName;
	bool bInitialized = false;
};

typedef TSharedRef<class FGitSourceControlChangelist, ESPMode::ThreadSafe> FGitSourceControlChangelistRef;
