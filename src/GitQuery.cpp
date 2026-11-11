#include "GitQuery.h"

#include <windows.h>

#include <string>
#include <vector>

namespace
{
constexpr DWORD kGitTimeoutMs = 15000;

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
            const bool absolute = (dir.size() >= 3 && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/')) ||
                                  (dir.size() >= 2 && dir[0] == L'\\' && dir[1] == L'\\');
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

const std::wstring& GitPath()
{
    static const std::wstring path = LocateGit();
    return path;
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

// Runs git in `workingDir`, capturing stdout. Returns false on failure,
// non-zero exit code or timeout.
// `allowExitCodeOne` accepts exit code 1, which `git config --get-regexp`
// uses for "no matches".
bool RunGit(const std::wstring& workingDir, const std::wstring& args, std::string& output,
            bool allowExitCodeOne = false)
{
    const std::wstring& git = GitPath();
    if (git.empty())
        return false;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    Handle readPipe, writePipe;
    if (!CreatePipe(&readPipe.h, &writePipe.h, &sa, 0))
        return false;
    SetHandleInformation(readPipe.h, HANDLE_FLAG_INHERIT, 0);

    Handle nul;
    nul.h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul.h;
    si.hStdOutput = writePipe.h;
    si.hStdError = nul.h;

    std::wstring commandLine = L"\"" + git + L"\" " + args;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(git.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, workingDir.c_str(), &si,
                        &pi))
        return false;

    Handle process, thread;
    process.h = pi.hProcess;
    thread.h = pi.hThread;

    // Close our copy of the write end so ReadFile sees EOF when git exits.
    CloseHandle(writePipe.h);
    writePipe.h = nullptr;

    const ULONGLONG start = GetTickCount64();
    char buffer[4096];
    bool exited = false;
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.h, nullptr, 0, nullptr, &available, nullptr))
            break; // Pipe closed: git has exited.

        if (available > 0)
        {
            DWORD read = 0;
            if (!ReadFile(readPipe.h, buffer, sizeof(buffer), &read, nullptr) || read == 0)
                break;
            output.append(buffer, read);
            continue;
        }

        if (WaitForSingleObject(process.h, 20) == WAIT_OBJECT_0)
        {
            exited = true;
            // Drain anything written just before exit.
            DWORD read = 0;
            while (PeekNamedPipe(readPipe.h, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
                   ReadFile(readPipe.h, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                output.append(buffer, read);
            break;
        }

        if (GetTickCount64() - start > kGitTimeoutMs)
        {
            TerminateProcess(process.h, 1);
            return false;
        }
    }

    if (!exited && WaitForSingleObject(process.h, kGitTimeoutMs) != WAIT_OBJECT_0)
    {
        TerminateProcess(process.h, 1);
        return false;
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.h, &exitCode))
        return false;
    return exitCode == 0 || (allowExitCodeOne && exitCode == 1);
}
} // namespace

RepoStatus QueryRepoStatus(const std::wstring& folder)
{
    std::wstring dotGit = folder;
    if (!dotGit.empty() && dotGit.back() != L'\\')
        dotGit += L'\\';
    dotGit += L".git";
    if (!FileExists(dotGit))
        return RepoStatus::NotRepo;

    // Explorer runs this for any folder the user merely browses to, e.g. an
    // extracted download. Never let repository-controlled configuration run
    // commands: filter drivers defined in the repository's own config are
    // refused (user/system-wide ones such as Git LFS are trusted), fsmonitor
    // hooks are disabled, and submodule work trees are not inspected.
    std::string filters;
    if (!RunGit(folder,
                L"--no-optional-locks config --includes --show-scope --get-regexp "
                L"\"^filter\\..*\\.(clean|process)$\"",
                filters, true))
        return RepoStatus::Error;
    if (HasRepositoryScopedEntries(filters))
        return RepoStatus::Error;

    // --no-optional-locks stops git refreshing the index, so we never hold
    // index.lock and interfere with git commands the user is running.
    std::string output;
    if (!RunGit(folder,
                L"--no-optional-locks -c core.fsmonitor=false status --porcelain=v2 --branch "
                L"--ignore-submodules=dirty",
                output))
        return RepoStatus::Error;

    return ParsePorcelainV2(output);
}
