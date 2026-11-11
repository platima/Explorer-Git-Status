#include "GitStatus.h"

#include <sstream>

RepoStatus ParsePorcelainV2(const std::string& output)
{
    bool aheadBehind = false;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        if (line[0] == '#')
        {
            // Header, e.g. "# branch.ab +1 -0"
            const std::string prefix = "# branch.ab ";
            if (line.compare(0, prefix.size(), prefix) == 0)
            {
                std::istringstream ab(line.substr(prefix.size()));
                long long ahead = 0, behind = 0;
                if (ab >> ahead >> behind)
                {
                    if (ahead != 0 || behind != 0)
                        aheadBehind = true;
                }
            }
            continue;
        }

        // Entry lines: '1' (changed), '2' (renamed/copied), 'u' (unmerged),
        // '?' (untracked). Ignored files ('!') are only listed on request.
        switch (line[0])
        {
        case '1':
        case '2':
        case 'u':
        case '?':
            return RepoStatus::Modified;
        default:
            break;
        }
    }

    return aheadBehind ? RepoStatus::AheadBehind : RepoStatus::Clean;
}

const wchar_t* const kUntrustedConfigPattern =
    L"^(filter\\..*\\.(clean|process)"
    L"|lfs\\.extension\\..*"
    L"|include\\..*"
    L"|includeif\\..*"
    L"|core\\.(excludesfile|attributesfile))$";

bool HasRepositoryScopedEntries(const std::string& configOutput)
{
    std::istringstream stream(configOutput);
    std::string line;
    while (std::getline(stream, line))
    {
        const size_t tab = line.find('\t');
        const std::string scope = line.substr(0, tab);
        if (scope == "local" || scope == "worktree")
            return true;
    }
    return false;
}

bool IsUncPath(const std::wstring& path)
{
    return path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
}

bool IsUncGitdirPointer(const std::string& dotGitFile)
{
    // Git reads `gitdir: <path>` from the start of the file and strips the
    // trailing line ending.
    const std::string prefix = "gitdir: ";
    if (dotGitFile.compare(0, prefix.size(), prefix) != 0)
        return false;
    const size_t begin = prefix.size();
    return dotGitFile.size() >= begin + 2 && (dotGitFile[begin] == '\\' || dotGitFile[begin] == '/') &&
           (dotGitFile[begin + 1] == '\\' || dotGitFile[begin + 1] == '/');
}
