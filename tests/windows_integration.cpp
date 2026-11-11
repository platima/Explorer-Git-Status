// Windows-only integration tests.
//
// These run the real git.exe against throwaway repositories and load the built
// DLL by path, so nothing is registered and Explorer is not involved. They
// cover the code the portable tests in tests.cpp cannot: GitQuery.cpp (process
// handling and the untrusted-repository checks) and the COM classes.
//
// usage: windows_integration <path to ExplorerGitStatus.dll>
//
// Exit code 77 means "skipped" (git.exe was not found).

#include "GitQuery.h"
#include "GitStatus.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(expr)                                                                \
    do                                                                             \
    {                                                                              \
        if (!(expr))                                                               \
        {                                                                          \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);          \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_STATUS(actual, expected)                                                         \
    do                                                                                         \
    {                                                                                          \
        const RepoStatus actual_ = (actual);                                                   \
        if (actual_ != (expected))                                                             \
        {                                                                                      \
            std::printf("FAILED %s:%d: %s is %s, expected %s\n", __FILE__, __LINE__, #actual,  \
                        StatusName(actual_), StatusName(expected));                            \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

const char* StatusName(RepoStatus status)
{
    switch (status)
    {
    case RepoStatus::Unknown: return "Unknown";
    case RepoStatus::NotRepo: return "NotRepo";
    case RepoStatus::Error: return "Error";
    case RepoStatus::Clean: return "Clean";
    case RepoStatus::Modified: return "Modified";
    case RepoStatus::AheadBehind: return "AheadBehind";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Small file and process helpers
// ---------------------------------------------------------------------------

std::wstring g_root;   // Scratch directory for this run.
std::wstring g_gitLog; // Everything git printed during fixture setup.

bool Exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == text.size();
}

std::string ReadTextFile(const std::wstring& path)
{
    std::string text;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return text;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        text.append(buffer, read);
    CloseHandle(h);
    return text;
}

// Same path with forward slashes, which is what git's shell wants inside a
// filter or hook command.
std::wstring Forward(std::wstring path)
{
    for (wchar_t& c : path)
        if (c == L'\\')
            c = L'/';
    return path;
}

std::string Utf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], size, nullptr, nullptr);
    return result;
}

// Runs `commandLine` in `dir`. Error output goes to the git log; standard
// output goes there too unless `stdoutPath` is given. Returns the exit code, or
// -1 if the process could not be started or timed out.
int RunProcess(const std::wstring& dir, const std::wstring& commandLine, const std::wstring* stdoutPath = nullptr)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW(g_gitLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE out = log;
    if (stdoutPath)
        out = CreateFileW(stdoutPath->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE)
    {
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (out != INVALID_HANDLE_VALUE && out != log)
            CloseHandle(out);
        return -1;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out;
    si.hStdError = log;

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION pi = {};
    int result = -1;
    if (CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       dir.c_str(), &si, &pi))
    {
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0)
        {
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            result = static_cast<int>(code);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(log);
    CloseHandle(nul);
    if (out != log)
        CloseHandle(out);
    return result;
}

bool Git(const std::wstring& dir, const std::wstring& args)
{
    const int code = RunProcess(dir, L"git " + args);
    if (code != 0)
    {
        std::printf("git command failed (exit %d): git %ls\n%s\n", code, args.c_str(), ReadTextFile(g_gitLog).c_str());
        return false;
    }
    return true;
}

// Runs git and returns its standard output with trailing whitespace removed.
std::string GitOutput(const std::wstring& dir, const std::wstring& args)
{
    const std::wstring outPath = g_root + L".git-output.txt";
    std::string text;
    if (RunProcess(dir, L"git " + args, &outPath) == 0)
        text = ReadTextFile(outPath);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

// Makes `data.txt` (committed), then arranges for plain `git status` to have to
// read its content again: a new timestamp with identical content, which makes
// git run the clean filter named by .gitattributes.
void PrepareFilteredFile(const std::wstring& repo, const std::string& attribute)
{
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));
    CHECK(Git(repo, L"add data.txt"));
    CHECK(Git(repo, L"commit -q -m data"));
    CHECK(WriteTextFile(repo + L"\\.gitattributes", attribute));
}

void TouchFilteredFile(const std::wstring& repo)
{
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));
}

std::wstring MakeDir(const std::wstring& name)
{
    const std::wstring path = g_root + L"\\" + name;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

// A repository with one commit and nothing else: status Clean, no upstream.
std::wstring MakeRepo(const std::wstring& name)
{
    const std::wstring dir = MakeDir(name);
    CHECK(Git(dir, L"init -q"));
    CHECK(WriteTextFile(dir + L"\\a.txt", "one\n"));
    CHECK(Git(dir, L"add a.txt"));
    CHECK(Git(dir, L"commit -q -m initial"));
    return dir;
}

// ---------------------------------------------------------------------------
// QueryRepoStatus against real git
// ---------------------------------------------------------------------------

void TestPlainFolders()
{
    const std::wstring plain = MakeDir(L"plain");
    CHECK_STATUS(QueryRepoStatus(plain), RepoStatus::NotRepo);

    // Only the folder that directly contains .git is a repository root.
    const std::wstring repo = MakeRepo(L"root-only");
    const std::wstring sub = repo + L"\\sub";
    CreateDirectoryW(sub.c_str(), nullptr);
    CHECK_STATUS(QueryRepoStatus(sub), RepoStatus::NotRepo);

    // A .git folder that is not a repository is reported as an error, not a crash.
    const std::wstring broken = MakeDir(L"broken");
    CreateDirectoryW((broken + L"\\.git").c_str(), nullptr);
    CHECK_STATUS(QueryRepoStatus(broken), RepoStatus::Error);

    // ...even inside a real repository: git must not walk up out of the broken
    // folder and report the enclosing repository's status as its own.
    const std::wstring nested = repo + L"\\nested-broken";
    CreateDirectoryW(nested.c_str(), nullptr);
    CreateDirectoryW((nested + L"\\.git").c_str(), nullptr);
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean); // Sanity: the enclosing repo is clean.
    CHECK_STATUS(QueryRepoStatus(nested), RepoStatus::Error);

    // A .git file that points at a network share is refused without running git.
    const std::wstring remote = MakeDir(L"unc-pointer");
    CHECK(WriteTextFile(remote + L"\\.git", "gitdir: //unreachable.invalid/share/repo.git\n"));
    CHECK_STATUS(QueryRepoStatus(remote), RepoStatus::Error);
}

// The test above passes even without the check, because git can't reach that
// host anyway. Here the pointer goes to a share that works: this machine's own
// administrative share. Plain git follows it happily, so only the extension's
// refusal can produce the error.
void TestGitdirPointerToWorkingNetworkShare()
{
    const std::wstring real = MakeRepo(L"unc-real");
    const std::wstring gitDir = real + L"\\.git";
    if (gitDir.size() < 3 || gitDir[1] != L':')
        return;

    // C:\a\b\.git becomes //localhost/C$/a/b/.git
    const std::wstring share = L"//localhost/" + std::wstring(1, gitDir[0]) + L"$" + Forward(gitDir.substr(2));
    const std::wstring pointer = MakeDir(L"unc-pointer-working");
    CHECK(WriteTextFile(pointer + L"\\.git", "gitdir: " + Utf8(share) + "\n"));

    DeleteFileW(g_gitLog.c_str());
    if (RunProcess(pointer, L"git status --porcelain") != 0)
    {
        std::string why = ReadTextFile(g_gitLog);
        if (why.size() > 400)
            why = why.substr(why.size() - 400);
        std::printf("note: the administrative share can't be used here, skipping the network gitdir check (%s)\n",
                    why.c_str());
        return;
    }
    CHECK_STATUS(QueryRepoStatus(pointer), RepoStatus::Error);
}

void TestWorkingTreeChanges()
{
    const std::wstring repo = MakeRepo(L"changes");
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);

    // Untracked.
    CHECK(WriteTextFile(repo + L"\\untracked.txt", "x\n"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Modified);
    DeleteFileW((repo + L"\\untracked.txt").c_str());
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);

    // Unstaged edit, then staged, then committed.
    CHECK(WriteTextFile(repo + L"\\a.txt", "two\n"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Modified);
    CHECK(Git(repo, L"add a.txt"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Modified);
    CHECK(Git(repo, L"commit -q -m second"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);

    // Ignored files do not count.
    CHECK(WriteTextFile(repo + L"\\.gitignore", "ignored.txt\n"));
    CHECK(Git(repo, L"add .gitignore"));
    CHECK(Git(repo, L"commit -q -m ignore"));
    CHECK(WriteTextFile(repo + L"\\ignored.txt", "x\n"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);
}

void TestAheadAndBehind()
{
    const std::wstring upstream = MakeDir(L"upstream.git");
    CHECK(Git(upstream, L"init -q --bare"));

    const std::wstring one = g_root + L"\\one";
    CHECK(Git(g_root, L"clone -q \"" + upstream + L"\" one"));
    CHECK(WriteTextFile(one + L"\\a.txt", "one\n"));
    CHECK(Git(one, L"add a.txt"));
    CHECK(Git(one, L"commit -q -m first"));
    CHECK(Git(one, L"push -q -u origin HEAD"));
    CHECK_STATUS(QueryRepoStatus(one), RepoStatus::Clean);

    // Ahead of upstream.
    CHECK(WriteTextFile(one + L"\\b.txt", "b\n"));
    CHECK(Git(one, L"add b.txt"));
    CHECK(Git(one, L"commit -q -m local"));
    CHECK_STATUS(QueryRepoStatus(one), RepoStatus::AheadBehind);
    CHECK(Git(one, L"push -q"));
    CHECK_STATUS(QueryRepoStatus(one), RepoStatus::Clean);

    // Behind upstream: another clone pushes, this one fetches.
    const std::wstring two = g_root + L"\\two";
    CHECK(Git(g_root, L"clone -q \"" + upstream + L"\" two"));
    CHECK(WriteTextFile(two + L"\\c.txt", "c\n"));
    CHECK(Git(two, L"add c.txt"));
    CHECK(Git(two, L"commit -q -m remote"));
    CHECK(Git(two, L"push -q"));
    CHECK(Git(one, L"fetch -q"));
    CHECK_STATUS(QueryRepoStatus(one), RepoStatus::AheadBehind);

    // A working tree change wins over ahead/behind.
    CHECK(WriteTextFile(one + L"\\untracked.txt", "x\n"));
    CHECK_STATUS(QueryRepoStatus(one), RepoStatus::Modified);
}

void TestLinkedWorktree()
{
    const std::wstring repo = MakeRepo(L"main-worktree");
    const std::wstring linked = g_root + L"\\linked-worktree";
    CHECK(Git(repo, L"worktree add -q \"" + linked + L"\" -b linked"));

    // In a linked worktree .git is a file, not a folder.
    const DWORD attributes = GetFileAttributesW((linked + L"\\.git").c_str());
    CHECK(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
    CHECK_STATUS(QueryRepoStatus(linked), RepoStatus::Clean);

    CHECK(WriteTextFile(linked + L"\\untracked.txt", "x\n"));
    CHECK_STATUS(QueryRepoStatus(linked), RepoStatus::Modified);
}

void TestUnusualFolderName()
{
    // Spaces and non-ASCII characters must survive the trip through
    // CreateProcess as the working directory.
    const std::wstring repo = MakeRepo(L"sp ace café リポ");
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);
    CHECK(WriteTextFile(repo + L"\\untracked.txt", "x\n"));
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Modified);
}

// Repository configuration is controlled by whoever made the repository, and
// Explorer runs the check just because the folder was browsed to. These tests
// plant commands that leave a marker file if they are run, and check that the
// extension never runs them. Each one first proves the plant works by running
// plain `git status`, so a passing test cannot be a false negative.
void TestUntrustedFilterIsNeverRun()
{
    const std::wstring repo = MakeRepo(L"evil-filter");
    const std::wstring marker = g_root + L"\\marker-filter.txt";
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));
    CHECK(Git(repo, L"add data.txt"));
    CHECK(Git(repo, L"commit -q -m data"));

    CHECK(WriteTextFile(repo + L"\\.gitattributes", "data.txt filter=evil\n"));
    CHECK(Git(repo, L"config filter.evil.clean \"echo ran >> '" + Forward(marker) + L"'\""));
    // Same content, new timestamp: git has to run the clean filter to compare.
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));

    RunProcess(repo, L"git status --porcelain"); // Exit status does not matter here.
    CHECK(Exists(marker));                        // Control: git itself does run the filter.
    DeleteFileW(marker.c_str());

    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Error);
    CHECK(!Exists(marker));
}

void TestUntrustedIncludedFilterIsNeverRun()
{
    const std::wstring repo = MakeRepo(L"evil-include");
    const std::wstring marker = g_root + L"\\marker-include.txt";
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));
    CHECK(Git(repo, L"add data.txt"));
    CHECK(Git(repo, L"commit -q -m data"));

    // The filter is only defined in a file pulled in by include.path.
    CHECK(WriteTextFile(repo + L"\\evil.inc",
                        "[filter \"evil\"]\n\tclean = echo ran >> '" + Utf8(Forward(marker)) + "'\n"));
    CHECK(WriteTextFile(repo + L"\\.gitattributes", "data.txt filter=evil\n"));
    CHECK(Git(repo, L"config include.path ../evil.inc"));
    CHECK(WriteTextFile(repo + L"\\data.txt", "hello\n"));

    RunProcess(repo, L"git status --porcelain");
    CHECK(Exists(marker));
    DeleteFileW(marker.c_str());

    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Error);
    CHECK(!Exists(marker));
}

void TestUntrustedFsmonitorIsNeverRun()
{
    const std::wstring repo = MakeRepo(L"evil-fsmonitor");
    const std::wstring marker = g_root + L"\\marker-fsmonitor.txt";
    CHECK(Git(repo, L"config core.fsmonitor \"echo ran >> '" + Forward(marker) + L"'\""));

    RunProcess(repo, L"git status --porcelain");
    CHECK(Exists(marker));
    DeleteFileW(marker.c_str());

    // The status is still reported; fsmonitor is just switched off for the call.
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);
    CHECK(!Exists(marker));
}

// Git LFS runs the commands in lfs.extension.<name>.clean from the repository's
// own config, through the (trusted) global LFS filter. Needs git-lfs; without
// it the control step cannot show the plant works and the test says so.
void TestUntrustedLfsExtensionIsNeverRun()
{
    const std::wstring repo = MakeRepo(L"evil-lfs");
    const std::wstring marker = g_root + L"\\marker-lfs.txt";
    PrepareFilteredFile(repo, "data.txt filter=lfs\n");
    // Git LFS runs extension commands directly (cmd style), not through git's
    // sh, so the command has to be one Windows can run on its own.
    const std::wstring command =
        L"powershell -NoProfile -Command Set-Content -Path '" + marker + L"' -Value ran";
    CHECK(Git(repo, L"config lfs.extension.evil.clean \"" + command + L"\""));
    CHECK(Git(repo, L"config lfs.extension.evil.smudge cat"));
    CHECK(Git(repo, L"config lfs.extension.evil.priority 0"));
    TouchFilteredFile(repo);

    RunProcess(repo, L"git status --porcelain");
    if (!Exists(marker))
    {
        // Hosted CI runners do have Git LFS, so a silent skip there would hide a
        // broken test. Elsewhere it is only a note.
        if (GetEnvironmentVariableW(L"GITHUB_ACTIONS", nullptr, 0) > 0)
        {
            std::printf("FAILED: the Git LFS plant did not run on a CI runner\n");
            ++g_failures;
        }
        else
        {
            std::printf("note: git-lfs is not available here, skipping the lfs.extension check\n");
        }
        return;
    }
    DeleteFileW(marker.c_str());

    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Error);
    CHECK(!Exists(marker));
}

// A repository can declare a "promisor" remote and be missing objects, which
// makes plain `git status` fetch from that remote, running the repository's
// own transport command (core.sshCommand) on the way.
void TestUntrustedPromisorRemoteIsNeverContacted()
{
    const std::wstring repo = MakeRepo(L"evil-promisor");
    const std::wstring marker = g_root + L"\\marker-promisor.txt";

    // Delete the root tree and the index so that status needs the tree.
    const std::string tree = GitOutput(repo, L"rev-parse HEAD^{tree}");
    CHECK(tree.size() == 40);
    const std::wstring object = repo + L"\\.git\\objects\\" + std::wstring(tree.begin(), tree.begin() + 2) + L"\\" +
                                std::wstring(tree.begin() + 2, tree.end());
    SetFileAttributesW(object.c_str(), FILE_ATTRIBUTE_NORMAL);
    CHECK(DeleteFileW(object.c_str()) != 0);
    DeleteFileW((repo + L"\\.git\\index").c_str());

    CHECK(Git(repo, L"config remote.origin.url ssh://unreachable.invalid/repo.git"));
    CHECK(Git(repo, L"config remote.origin.promisor true"));
    CHECK(Git(repo, L"config protocol.ssh.allow always"));
    CHECK(Git(repo, L"config core.sshCommand \"echo ran >> '" + Forward(marker) + L"'\""));

    RunProcess(repo, L"git status --porcelain");
    CHECK(Exists(marker)); // Control: git itself does try the fetch.
    DeleteFileW(marker.c_str());

    // The status itself cannot be computed (the tree is gone), but nothing runs.
    QueryRepoStatus(repo);
    CHECK(!Exists(marker));
}

// post-index-change runs whenever git writes the index. A status check must
// not write it.
void TestUntrustedIndexHookIsNeverRun()
{
    const std::wstring repo = MakeRepo(L"evil-hook");
    const std::wstring marker = g_root + L"\\marker-hook.txt";
    CHECK(WriteTextFile(repo + L"\\.git\\hooks\\post-index-change",
                        "#!/bin/sh\necho ran >> '" + Utf8(Forward(marker)) + "'\n"));
    // New timestamp, same content: plain status refreshes and rewrites the index.
    CHECK(WriteTextFile(repo + L"\\a.txt", "one\n"));

    RunProcess(repo, L"git status --porcelain");
    CHECK(Exists(marker)); // Control: git itself runs the hook.
    DeleteFileW(marker.c_str());
    CHECK(WriteTextFile(repo + L"\\a.txt", "one\n"));

    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Clean);
    CHECK(!Exists(marker));
}

// Output far larger than the reader keeps: the answer is still right, and the
// run ends quickly instead of waiting for git to finish listing everything.
void TestHugeStatusOutput()
{
    const std::wstring repo = MakeRepo(L"huge-output");
    for (int i = 0; i < 20000; ++i)
    {
        HANDLE h = CreateFileW((repo + L"\\untracked-file-" + std::to_wstring(i) + L".txt").c_str(), GENERIC_WRITE,
                               0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }

    const ULONGLONG start = GetTickCount64();
    CHECK_STATUS(QueryRepoStatus(repo), RepoStatus::Modified);
    CHECK(GetTickCount64() - start < 20000);
}

// ---------------------------------------------------------------------------
// The built DLL, loaded by path (no registration)
// ---------------------------------------------------------------------------

const CLSID kClsids[3] = {
    { 0xef9cc9e5, 0x741c, 0x4c53, { 0x8f, 0xb4, 0xd0, 0xc3, 0x17, 0xc3, 0x46, 0x4b } }, // Clean
    { 0xd5935fba, 0x49fe, 0x4239, { 0x8b, 0x45, 0xcd, 0x55, 0xa1, 0xf2, 0xec, 0xbc } }, // Modified
    { 0x76a9c9d1, 0xb37a, 0x4981, { 0x80, 0x87, 0xf6, 0xb5, 0x16, 0xee, 0xe4, 0x1c } }, // Ahead/behind
};
const CLSID kUnknownClsid = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);
typedef HRESULT(STDAPICALLTYPE* CanUnloadNowFn)();

// Which of the three handlers claim `path`: bit 0 Clean, 1 Modified, 2 AheadBehind.
unsigned Claims(IShellIconOverlayIdentifier* const handlers[3], const std::wstring& path, DWORD attributes)
{
    unsigned bits = 0;
    for (int i = 0; i < 3; ++i)
        if (handlers[i]->IsMemberOf(path.c_str(), attributes) == S_OK)
            bits |= 1u << i;
    return bits;
}

// The first IsMemberOf for a folder only queues the background check, so poll
// until the expected handler claims it.
bool EventuallyClaimedBy(IShellIconOverlayIdentifier* const handlers[3], const std::wstring& path, unsigned expected)
{
    for (int i = 0; i < 600; ++i)
    {
        if (Claims(handlers, path, FILE_ATTRIBUTE_DIRECTORY) == expected)
            return true;
        Sleep(50);
    }
    return false;
}

void TestDll(const std::wstring& dllPath)
{
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll)
    {
        std::printf("FAILED could not load %ls (error %lu)\n", dllPath.c_str(), GetLastError());
        ++g_failures;
        return;
    }

    GetClassObjectFn getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    CanUnloadNowFn canUnloadNow = reinterpret_cast<CanUnloadNowFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    CHECK(GetProcAddress(dll, "DllRegisterServer") != nullptr);
    CHECK(GetProcAddress(dll, "DllUnregisterServer") != nullptr);
    CHECK(getClassObject != nullptr && canUnloadNow != nullptr);
    if (!getClassObject || !canUnloadNow)
        return;

    // Class factories and handlers.
    void* none = nullptr;
    CHECK(getClassObject(kUnknownClsid, IID_IClassFactory, &none) == CLASS_E_CLASSNOTAVAILABLE && !none);

    IShellIconOverlayIdentifier* handlers[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        IClassFactory* factory = nullptr;
        CHECK(getClassObject(kClsids[i], IID_IClassFactory, reinterpret_cast<void**>(&factory)) == S_OK && factory);
        if (!factory)
            return;

        IUnknown* outer = factory; // Any non-null controlling unknown.
        void* aggregated = nullptr;
        CHECK(factory->CreateInstance(outer, IID_IShellIconOverlayIdentifier, &aggregated) == CLASS_E_NOAGGREGATION);
        const IID unsupported = { 0x0badf00d, 0x1111, 0x2222, { 3, 3, 3, 3, 3, 3, 3, 3 } };
        void* unsupportedPtr = nullptr;
        CHECK(factory->CreateInstance(nullptr, unsupported, &unsupportedPtr) == E_NOINTERFACE);

        CHECK(factory->CreateInstance(nullptr, IID_IShellIconOverlayIdentifier,
                                      reinterpret_cast<void**>(&handlers[i])) == S_OK &&
              handlers[i]);
        factory->Release();
        if (!handlers[i])
            return;
    }

    // Icon file, index and flags: the index must map to the matching icon in
    // the DLL's resources, in the order clean, modified, ahead/behind.
    wchar_t iconFile[MAX_PATH] = {};
    for (int i = 0; i < 3; ++i)
    {
        int index = -1;
        DWORD flags = 0;
        CHECK(handlers[i]->GetOverlayInfo(iconFile, MAX_PATH, &index, &flags) == S_OK);
        CHECK(index == i);
        CHECK(flags == (ISIOI_ICONFILE | ISIOI_ICONINDEX));

        int priority = -1;
        CHECK(handlers[i]->GetPriority(&priority) == S_OK);

        // (Not named "small": the Windows headers #define that to char.)
        HICON largeIcon = nullptr, smallIcon = nullptr;
        const UINT extracted = ExtractIconExW(iconFile, index, &largeIcon, &smallIcon, 1);
        CHECK(extracted >= 1 && (largeIcon || smallIcon));
        if (largeIcon)
            DestroyIcon(largeIcon);
        if (smallIcon)
            DestroyIcon(smallIcon);
    }
    CHECK(ExtractIconExW(iconFile, -1, nullptr, nullptr, 0) == 3);

    // Each folder is claimed by exactly the handler for its status.
    const std::wstring clean = MakeRepo(L"com-clean");

    const std::wstring modified = MakeRepo(L"com-modified");
    CHECK(WriteTextFile(modified + L"\\untracked.txt", "x\n"));

    const std::wstring upstream = MakeDir(L"com-upstream.git");
    CHECK(Git(upstream, L"init -q --bare"));
    const std::wstring ahead = g_root + L"\\com-ahead";
    CHECK(Git(g_root, L"clone -q \"" + upstream + L"\" com-ahead"));
    CHECK(WriteTextFile(ahead + L"\\a.txt", "one\n"));
    CHECK(Git(ahead, L"add a.txt"));
    CHECK(Git(ahead, L"commit -q -m first"));
    CHECK(Git(ahead, L"push -q -u origin HEAD"));
    CHECK(WriteTextFile(ahead + L"\\b.txt", "b\n"));
    CHECK(Git(ahead, L"add b.txt"));
    CHECK(Git(ahead, L"commit -q -m local"));

    CHECK(EventuallyClaimedBy(handlers, clean, 1u << 0));
    CHECK(EventuallyClaimedBy(handlers, modified, 1u << 1));
    CHECK(EventuallyClaimedBy(handlers, ahead, 1u << 2));

    // Attributes that must never be probed or badged. These are repositories
    // that are already cached, so a wrong answer would show up straight away.
    CHECK(Claims(handlers, clean, FILE_ATTRIBUTE_NORMAL) == 0);
    CHECK(Claims(handlers, clean, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE) == 0);
    CHECK(Claims(handlers, L"", FILE_ATTRIBUTE_DIRECTORY) == 0);

    // Plain folders never get a badge, however long we wait.
    const std::wstring plain = MakeDir(L"com-plain");
    for (int i = 0; i < 40; ++i)
    {
        CHECK(Claims(handlers, plain, FILE_ATTRIBUTE_DIRECTORY) == 0);
        Sleep(50);
    }

    for (int i = 0; i < 3; ++i)
        handlers[i]->Release();

    // The status cache has a live worker thread, so the DLL must stay loaded.
    // (It is deliberately never freed here either.)
    CHECK(canUnloadNow() == S_FALSE);
}

void Cleanup()
{
    RunProcess(g_root, L"cmd.exe /c rd /s /q \"" + g_root + L"\"");
}
} // namespace

int main()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2)
    {
        std::printf("usage: windows_integration <path to ExplorerGitStatus.dll>\n");
        return 2;
    }
    const std::wstring dllPath = argv[1];
    LocalFree(argv);

    wchar_t temp[MAX_PATH];
    const DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH)
        return 2;
    g_root = std::wstring(temp) + L"ExplorerGitStatusTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(g_root.c_str(), nullptr))
    {
        std::printf("could not create %ls\n", g_root.c_str());
        return 2;
    }
    g_gitLog = g_root + L".git-setup.log";

    // Make the runs independent of the machine's own git configuration. The
    // global file below also defines Git LFS style filters: filters from the
    // user's own configuration must keep working.
    const std::wstring globalConfig = g_root + L".gitconfig";
    WriteTextFile(globalConfig,
                  "[user]\n\tname = Test\n\temail = test@example.invalid\n"
                  "[init]\n\tdefaultBranch = main\n"
                  "[core]\n\tautocrlf = false\n"
                  "[protocol \"file\"]\n\tallow = always\n"
                  // Many people set this. Git then follows a .git pointer to a
                  // network share, which is why the extension refuses those itself.
                  "[safe]\n\tdirectory = *\n"
                  "[filter \"lfs\"]\n\tclean = git-lfs clean -- %f\n\tprocess = git-lfs filter-process\n");
    SetEnvironmentVariableW(L"GIT_CONFIG_GLOBAL", globalConfig.c_str());
    SetEnvironmentVariableW(L"GIT_CONFIG_NOSYSTEM", L"1");
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");

    if (RunProcess(g_root, L"git --version") != 0)
    {
        std::printf("git.exe not found, skipping\n");
        Cleanup();
        return 77;
    }

    TestPlainFolders();
    TestGitdirPointerToWorkingNetworkShare();
    TestWorkingTreeChanges();
    TestAheadAndBehind();
    TestLinkedWorktree();
    TestUnusualFolderName();
    TestUntrustedFilterIsNeverRun();
    TestUntrustedIncludedFilterIsNeverRun();
    TestUntrustedFsmonitorIsNeverRun();
    TestUntrustedLfsExtensionIsNeverRun();
    TestUntrustedPromisorRemoteIsNeverContacted();
    TestUntrustedIndexHookIsNeverRun();
    TestHugeStatusOutput();
    TestDll(dllPath);

    if (g_failures == 0)
    {
        std::printf("All Windows integration tests passed\n");
        Cleanup();
    }
    else
    {
        std::printf("%d check(s) failed; scratch files kept in %ls\n", g_failures, g_root.c_str());
    }
    return g_failures == 0 ? 0 : 1;
}
