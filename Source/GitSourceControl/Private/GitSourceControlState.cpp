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
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
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
	if (History.IsEmpty())
	{
		return nullptr;
	}
	return History[0];
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
	case EGitState::Unmerged:
		return GET_ICON_RETURN(Branched);
	case EGitState::Added:
		return GET_ICON_RETURN(OpenForAdd);
	case EGitState::Untracked:
		return GET_ICON_RETURN(NotInDepot);
	case EGitState::Deleted:
		return GET_ICON_RETURN(MarkedForDelete);
	case EGitState::Modified:
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
	case EGitState::Unmerged:
		return LOCTEXT("Conflicted", "Conflicted");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd", "Opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled", "Not Under Revision Control");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete", "Marked for delete");
	case EGitState::Modified:
		return LOCTEXT("Modified", "Modified");
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
	case EGitState::Unmerged:
		return LOCTEXT("ContentsConflict_Tooltip", "The contents of the item conflict with updates received from the repository.");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd_Tooltip", "The file(s) are opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled_Tooltip", "Item is not under revision control.");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete_Tooltip", "The file(s) are marked for delete");
	case EGitState::Modified:
		return LOCTEXT("Modified_Tooltip", "The file has local Git changes.");
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
	return false;
}

bool FGitSourceControlState::CanCheckout() const
{
	return false;
}

bool FGitSourceControlState::IsCheckedOut() const
{
	return false;
}

bool FGitSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who != nullptr)
	{
		Who->Reset();
	}
	return false;
}

bool FGitSourceControlState::IsCheckedOutInOtherBranch(const FString& CurrentBranch) const
{
	// You can't check out separately per branch
	return false;
}

bool FGitSourceControlState::IsModifiedInOtherBranch(const FString& CurrentBranch) const
{
	return false;
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
	return true;
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
	return !IsConflicted() && !IsIgnored() && State.TreeState != ETreeState::NotInRepo;
}

bool FGitSourceControlState::CanDelete() const
{
	return IsSourceControlled() && !IsConflicted() && !IsIgnored();
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
	return false;
}

bool FGitSourceControlState::IsConflicted() const
{
	return State.FileState == EFileState::Unmerged;
}

bool FGitSourceControlState::CanRevert() const
{
	// Generic UE Revert has no asset-aware confirmation or reload boundary.
	return false;
}

EGitState::Type FGitSourceControlState::GetGitState() const
{
	switch (State.FileState)
	{
	case EFileState::Unmerged:
		return EGitState::Unmerged;
	case EFileState::Added:
		return EGitState::Added;
	case EFileState::Deleted:
		return EGitState::Deleted;
	case EFileState::Modified:
	case EFileState::Copied:
	case EFileState::Renamed:
		return EGitState::Modified;
	default:
		break;
	}

	if (State.TreeState == ETreeState::Untracked)
	{
		return EGitState::Untracked;
	}
	if (State.TreeState == ETreeState::Ignored)
	{
		return EGitState::Ignored;
	}

	if (IsSourceControlled())
	{
		return EGitState::Unmodified;
	}

	return EGitState::None;
}

#undef LOCTEXT_NAMESPACE
