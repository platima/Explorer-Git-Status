#pragma once

#include <string>

// Summarised state of a folder as far as git is concerned.
enum class RepoStatus
{
    Unknown,     // Not checked yet (or the check is still running).
    NotRepo,     // Folder is not the root of a git repository / worktree.
    Error,       // It is a repository, but `git status` failed.
    Clean,       // Working tree clean and in sync with upstream (or no upstream).
    Modified,    // Staged, unstaged, untracked or conflicted changes present.
    AheadBehind, // Working tree clean, but local branch differs from upstream.
};

// Parses the output of `git status --porcelain=v2 --branch` into a RepoStatus.
// Portable (no Windows dependencies) so it can be unit tested anywhere.
RepoStatus ParsePorcelainV2(const std::string& output);

// Given the output of `git config --show-scope --get-regexp ...`, returns true
// if any entry comes from the repository's own configuration ("local" or
// "worktree" scope), i.e. is controlled by whoever produced the repository.
bool HasRepositoryScopedEntries(const std::string& configOutput);
