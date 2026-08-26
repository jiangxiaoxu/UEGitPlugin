// Copyright (c) 2026

#pragma once

#include "CoreMinimal.h"

/** Lightweight, transient status used only to validate an explicit asset mutation. */
namespace EGitFileState
{
	enum Type
	{
		Unknown,
		Added,
		Copied,
		Deleted,
		Modified,
		Renamed,
		Unmerged,
	};
}

namespace EGitTreeState
{
	enum Type
	{
		Unmodified,
		Working,
		Staged,
		Untracked,
		Ignored,
		NotInRepo,
	};
}

struct GITSOURCECONTROL_API FGitSourceControlFileStatus
{
	EGitFileState::Type FileState = EGitFileState::Unknown;
	EGitTreeState::Type TreeState = EGitTreeState::NotInRepo;

	bool IsConflicted() const
	{
		return FileState == EGitFileState::Unmerged;
	}

	bool IsTracked() const
	{
		return TreeState != EGitTreeState::Untracked && TreeState != EGitTreeState::Ignored && TreeState != EGitTreeState::NotInRepo;
	}
};
