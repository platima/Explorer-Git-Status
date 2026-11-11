#pragma once

#include "GitStatus.h"

#include <string>

// Determines the git status of `folder` by running
// `git status --porcelain=v2 --branch` in it. Only folders that directly
// contain a `.git` directory (or `.git` file, for worktrees/submodules) are
// treated as repositories. This blocks, so must only be called from a
// background thread.
RepoStatus QueryRepoStatus(const std::wstring& folder);
