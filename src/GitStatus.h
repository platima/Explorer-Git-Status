#pragma once

#include <string>

// Summarised state of a folder as far as git is concerned.
enum class RepoStatus
{
    Unknown,     // Not checked yet (or the check is still running).
    NotRepo,     // Folder is not the root of a git repository / worktree.
    Error,       // It is a repository, but `git status` failed or was refused.
    Clean,       // Working tree clean and in sync with upstream (or no upstream).
    Modified,    // Staged, unstaged, untracked or conflicted changes present.
    AheadBehind, // Working tree clean, but local branch differs from upstream.
};

// True for the statuses that are drawn as a badge.
inline bool HasOverlay(RepoStatus status)
{
    return status == RepoStatus::Clean || status == RepoStatus::Modified || status == RepoStatus::AheadBehind;
}

// Parses the output of `git status --porcelain=v2 --branch` into a RepoStatus.
// Portable (no Windows dependencies) so it can be unit tested anywhere.
RepoStatus ParsePorcelainV2(const std::string& output);

// Regular expression (for `git config --get-regexp`) matching every setting
// that lets a repository's own configuration run a program, or point git at a
// file somewhere else, while we only want to read its status:
//   * filter.*.clean / filter.*.process: programs run to compare file content
//   * lfs.extension.*: Git LFS runs these from the repository's config
//   * include.* / includeIf.*: pull in more configuration from any path
//   * core.excludesFile / core.attributesFile: read files from any path
extern const wchar_t* const kUntrustedConfigPattern;

// Given the output of `git config --show-scope --get-regexp <pattern>`,
// returns true if any entry comes from the repository's own configuration
// ("local" or "worktree" scope), i.e. is controlled by whoever produced the
// repository.
bool HasRepositoryScopedEntries(const std::string& configOutput);

// True for UNC and device paths (`\\server\share`, `//server/share`,
// `\\?\...`). Probing these can block for tens of seconds when the host is
// unreachable, and a repository must not be able to steer git at one.
bool IsUncPath(const std::wstring& path);

// Given the contents of a `.git` file (`gitdir: <path>`), returns true if the
// path it points at is a UNC path.
bool IsUncGitdirPointer(const std::string& dotGitFile);
