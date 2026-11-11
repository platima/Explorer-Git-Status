#include "GitQuery.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace
{
constexpr DWORD kGitTimeoutMs = 15000;

// Most output we keep. `git status --porcelain=v2 --branch` lists one line per
// changed file, and the first entry line already decides the badge, so there is
// no point reading (or waiting for) the rest of a very large listing.
constexpr size_t kStatusOutputLimit = 64 * 1024;
// The config query only returns the few settings in kUntrustedConfigPattern.
// If it returns more than this something is wrong, so it is treated as a failure.
constexpr size_t kConfigOutputLimit = 1024 * 1024;

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExpandEnv(const wchar_t* value)
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD len = ExpandEnvironmentStringsW(value, buffer, ARRAYSIZE(buffer));
    if (len == 0 || len > ARRAYSIZE(buffer))
        return std::wstring();
    return buffer;
}

std::wstring LocateGit()
{
    // Search PATH explicitly rather than with SearchPath, which would also
    // look in the host process's current directory (binary planting).
    DWORD pathLen = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (pathLen > 0)
    {
        std::wstring pathVar(pathLen, L'\0');
        pathLen = GetEnvironmentVariableW(L"PATH", &pathVar[0], pathLen);
        pathVar.resize(pathLen);
        size_t begin = 0;
        while (begin <= pathVar.size())
        {
            size_t end = pathVar.find(L';', begin);
            if (end == std::wstring::npos)
                end = pathVar.size();
            std::wstring dir = pathVar.substr(begin, end - begin);
            begin = end + 1;

            if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"')
                dir = dir.substr(1, dir.size() - 2);
            // Local drives only. A UNC entry on a dead server would stall the
            // lookup, and git is not expected to live on a network share.
            const bool absolute = dir.size() >= 3 && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/');
            if (!absolute)
                continue;
            if (dir.back() != L'\\' && dir.back() != L'/')
                dir += L'\\';
            std::wstring candidate = dir + L"git.exe";
            if (FileExists(candidate))
                return candidate;
        }
    }

    // Explorer is not always started with Git on its PATH; try the usual
    // Git for Windows install locations.
    const wchar_t* candidates[] = {
        L"%ProgramW6432%\\Git\\cmd\\git.exe",
        L"%ProgramFiles%\\Git\\cmd\\git.exe",
        L"%ProgramFiles(x86)%\\Git\\cmd\\git.exe",
        L"%LOCALAPPDATA%\\Programs\\Git\\cmd\\git.exe",
    };
    for (const wchar_t* candidate : candidates)
    {
        std::wstring path = ExpandEnv(candidate);
        if (!path.empty() && path.find(L'%') == std::wstring::npos && FileExists(path))
            return path;
    }
    return std::wstring();
}

// The location of git.exe. A successful lookup is kept for the life of the
// process; a failed one is retried at most once a minute, so installing git
// while Explorer is running is picked up without a restart.
std::wstring GitPath()
{
    static std::mutex mutex;
    static std::wstring found;
    static ULONGLONG lastAttempt = 0;
    static bool attempted = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (!found.empty())
        return found;
    const ULONGLONG now = GetTickCount64();
    if (attempted && now - lastAttempt < 60 * 1000)
        return found;
    attempted = true;
    lastAttempt = now;
    found = LocateGit();
    return found;
}

struct Handle
{
    HANDLE h = nullptr;
    ~Handle()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};

// A process/thread attribute list that holds the handle whitelist.
struct AttributeList
{
    std::vector<char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    ~AttributeList()
    {
        if (list)
            DeleteProcThreadAttributeList(list);
    }

    bool InitWithHandles(HANDLE* handles, DWORD count)
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0)
            return false;
        storage.resize(size);
        LPPROC_THREAD_ATTRIBUTE_LIST candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size))
            return false;
        list = candidate;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                         count * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    }
};

bool EqualsIgnoreCase(const wchar_t* a, size_t aLen, const wchar_t* b)
{
    return CompareStringOrdinal(a, static_cast<int>(aLen), b, -1, TRUE) == CSTR_EQUAL;
}

// Builds the environment block for the git child process. It is the host's
// own environment with the git variables that could redirect or widen the
// query removed, and a few that keep git from reaching out added:
//   * GIT_DIR, GIT_WORK_TREE, ...: would make every folder report some other
//     repository.
//   * GIT_ALLOW_PROTOCOL=none, GIT_NO_LAZY_FETCH=1: a repository can declare a
//     "promisor" remote and lack objects, which makes `git status` fetch from it
//     and run the repository's own transport commands (core.sshCommand and
//     friends). Neither variable is read by the repository's config.
//   * GIT_CEILING_DIRECTORIES: stops git walking up out of an invalid `.git`
//     into an enclosing repository and reporting that one's status.
//   * GIT_TERMINAL_PROMPT=0: never wait for input.
std::vector<wchar_t> BuildGitEnvironment(const std::wstring& folder)
{
    static const wchar_t* const kReplaced[] = {
        L"GIT_DIR",
        L"GIT_WORK_TREE",
        L"GIT_INDEX_FILE",
        L"GIT_COMMON_DIR",
        L"GIT_OBJECT_DIRECTORY",
        L"GIT_ALTERNATE_OBJECT_DIRECTORIES",
        L"GIT_NAMESPACE",
        L"GIT_CEILING_DIRECTORIES",
        L"GIT_ALLOW_PROTOCOL",
        L"GIT_NO_LAZY_FETCH",
        L"GIT_TERMINAL_PROMPT",
    };

    std::vector<std::wstring> entries;
    if (LPWCH block = GetEnvironmentStringsW())
    {
        for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1)
        {
            // Names can start with '=' (hidden per-drive entries such as "=C:").
            const wchar_t* equals = wcschr(entry + 1, L'=');
            const size_t nameLen = equals ? static_cast<size_t>(equals - entry) : wcslen(entry);
            bool replaced = false;
            for (const wchar_t* name : kReplaced)
                replaced = replaced || EqualsIgnoreCase(entry, nameLen, name);
            if (!replaced)
                entries.emplace_back(entry);
        }
        FreeEnvironmentStringsW(block);
    }

    entries.emplace_back(L"GIT_ALLOW_PROTOCOL=none");
    entries.emplace_back(L"GIT_NO_LAZY_FETCH=1");
    entries.emplace_back(L"GIT_TERMINAL_PROMPT=0");

    // The ceiling is the parent of the folder, so git may look at the folder
    // itself but never above it. (A drive root has no parent.)
    std::wstring trimmed = folder;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.pop_back();
    if (trimmed.size() > 3)
    {
        const size_t slash = trimmed.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash >= 2)
        {
            // "C:\repo" has the parent "C:\", not "C:".
            const size_t length = slash == 2 ? 3 : slash;
            entries.emplace_back(L"GIT_CEILING_DIRECTORIES=" + trimmed.substr(0, length));
        }
    }

    // CreateProcess wants the block sorted by name, case-insensitively.
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    std::vector<wchar_t> result;
    for (const std::wstring& entry : entries)
    {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

// Runs git in `workingDir`, capturing stdout. Returns false on failure,
// non-zero exit code or timeout.
// `allowExitCodeOne` accepts exit code 1, which `git config --get-regexp`
// uses for "no matches".
// `maxBytes` (0 means no limit) stops the run once that much output has been
// read: git is killed, `*truncated` is set and the call still returns true.
bool RunGit(const std::wstring& workingDir, const std::wstring& args, std::string& output, bool allowExitCodeOne,
            size_t maxBytes, bool* truncated)
{
    if (truncated)
        *truncated = false;

    const std::wstring git = GitPath();
    if (git.empty())
        return false;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    Handle readPipe, writePipe;
    // A large pipe buffer lets git write without waiting for us to poll.
    if (!CreatePipe(&readPipe.h, &writePipe.h, &sa, 1024 * 1024))
        return false;
    SetHandleInformation(readPipe.h, HANDLE_FLAG_INHERIT, 0);

    Handle nul;
    nul.h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, 0, nullptr);
    if (nul.h == INVALID_HANDLE_VALUE)
        return false;

    // The DLL runs inside Explorer and other hosts, which may have all sorts of
    // inheritable handles open. Hand git only the two it needs; each handle
    // may appear in the list once, even though NUL is used for two streams.
    HANDLE inheritable[] = { writePipe.h, nul.h };
    AttributeList attributes;
    if (!attributes.InitWithHandles(inheritable, ARRAYSIZE(inheritable)))
        return false;

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul.h;
    si.StartupInfo.hStdOutput = writePipe.h;
    si.StartupInfo.hStdError = nul.h;
    si.lpAttributeList = attributes.list;

    std::wstring commandLine = L"\"" + git + L"\" " + args;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::vector<wchar_t> environment = BuildGitEnvironment(workingDir);

    // Git for Windows' cmd\git.exe is a launcher that starts the real git as a
    // child process, so run everything in a job object: closing the job (on
    // any return path) kills the whole process tree, e.g. after a timeout.
    // Jobs do not nest before Windows 8; if the host is already in one the
    // assignment fails and we carry on without, killing just the launcher.
    Handle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    bool jobAssigned = false;
    if (job.h)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            CloseHandle(job.h);
            job.h = nullptr;
        }
    }

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(git.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), workingDir.c_str(), &si.StartupInfo, &pi))
        return false;

    Handle process, thread;
    process.h = pi.hProcess;
    thread.h = pi.hThread;

    if (job.h)
        jobAssigned = AssignProcessToJobObject(job.h, process.h) != FALSE;

    auto kill = [&]() {
        if (jobAssigned)
            TerminateJobObject(job.h, 1);
        else
            TerminateProcess(process.h, 1);
    };

    if (ResumeThread(thread.h) == static_cast<DWORD>(-1))
    {
        kill();
        return false;
    }

    // Close our copy of the write end so ReadFile sees EOF when git exits.
    CloseHandle(writePipe.h);
    writePipe.h = nullptr;

    const ULONGLONG start = GetTickCount64();
    std::vector<char> buffer(64 * 1024);
    bool exited = false;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.h, nullptr, 0, nullptr, &available, nullptr))
            break; // Pipe closed: git has exited.

        if (available > 0)
        {
            DWORD read = 0;
            if (!ReadFile(readPipe.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0)
                break;
            output.append(buffer.data(), read);
            if (maxBytes != 0 && output.size() >= maxBytes)
            {
                kill();
                if (truncated)
                    *truncated = true;
                return true;
            }
            continue;
        }

        if (WaitForSingleObject(process.h, 20) == WAIT_OBJECT_0)
        {
            exited = true;
            // Drain anything written just before exit.
            DWORD read = 0;
            while (PeekNamedPipe(readPipe.h, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
                   ReadFile(readPipe.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
                   read > 0)
                output.append(buffer.data(), read);
            break;
        }

        if (GetTickCount64() - start > kGitTimeoutMs)
        {
            kill();
            return false;
        }
    }

    if (!exited)
    {
        const ULONGLONG elapsed = GetTickCount64() - start;
        const DWORD remaining = elapsed >= kGitTimeoutMs ? 0 : static_cast<DWORD>(kGitTimeoutMs - elapsed);
        if (WaitForSingleObject(process.h, remaining) != WAIT_OBJECT_0)
        {
            kill();
            return false;
        }
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.h, &exitCode))
        return false;
    return exitCode == 0 || (allowExitCodeOne && exitCode == 1);
}

// Reads up to `limit` bytes of a small file; empty on failure.
std::string ReadSmallFile(const std::wstring& path, size_t limit)
{
    std::string text;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return text;
    text.resize(limit);
    DWORD read = 0;
    if (!ReadFile(h, &text[0], static_cast<DWORD>(limit), &read, nullptr))
        read = 0;
    CloseHandle(h);
    text.resize(read);
    return text;
}
} // namespace

RepoStatus QueryRepoStatus(const std::wstring& folder)
{
    std::wstring dotGit = folder;
    if (!dotGit.empty() && dotGit.back() != L'\\')
        dotGit += L'\\';
    dotGit += L".git";
    const DWORD attributes = GetFileAttributesW(dotGit.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return RepoStatus::NotRepo;

    // A `.git` file (worktrees, submodules) points at the real git directory.
    // Git follows whatever it says, including a network share, which would
    // make Explorer contact a server of the repository author's choosing.
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && IsUncGitdirPointer(ReadSmallFile(dotGit, 4096)))
        return RepoStatus::Error;

    // Explorer runs this for any folder the user merely browses to, e.g. an
    // extracted download. Never let repository-controlled configuration run
    // commands or reach other files:
    //   * repositories whose own config defines filters, Git LFS extensions,
    //     includes, or excludes/attributes files are refused (settings from the
    //     user's global and system config, such as Git LFS, are trusted),
    //   * `--no-includes` keeps this query itself from following an include,
    //   * `--work-tree=.` ignores core.worktree, which git resolves before it
    //     reads any other setting and which can point anywhere,
    //   * fsmonitor and hooks are disabled, transports and lazy fetching are
    //     switched off through the environment (see BuildGitEnvironment), and
    //     submodule work trees are not inspected.
    std::string unsafeConfig;
    bool truncated = false;
    if (!RunGit(folder,
                std::wstring(L"--no-optional-locks --work-tree=. config --no-includes --show-scope --get-regexp \"") +
                    kUntrustedConfigPattern + L"\"",
                unsafeConfig, true, kConfigOutputLimit, &truncated) ||
        truncated)
        return RepoStatus::Error;
    if (HasRepositoryScopedEntries(unsafeConfig))
        return RepoStatus::Error;

    // --no-optional-locks stops git refreshing the index, so we never hold
    // index.lock and interfere with git commands the user is running. It also
    // means git never writes the index, so index hooks such as
    // post-index-change do not run; core.hooksPath is pointed at nothing as
    // well in case that ever changes.
    std::string output;
    if (!RunGit(folder,
                L"--no-optional-locks --work-tree=. -c core.fsmonitor=false -c core.hooksPath=NUL "
                L"status --porcelain=v2 --branch --ignore-submodules=dirty",
                output, false, kStatusOutputLimit, &truncated))
        return RepoStatus::Error;

    return ParsePorcelainV2(output);
}
