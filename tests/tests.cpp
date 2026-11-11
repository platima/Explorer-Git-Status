// Minimal self-contained tests for the portable core (no framework needed).
//
// The cache tests are deterministic: they wait on events rather than on the
// clock, and only use timeouts so that a bug fails the test instead of hanging
// it.

#include "GitStatus.h"
#include "StatusCache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(expr)                                                                \
    do                                                                             \
    {                                                                              \
        if (!(expr))                                                               \
        {                                                                          \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);          \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

namespace
{
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A latch: Set() once, Wait() until it is set (bounded, so a bug cannot hang).
class Event
{
public:
    void Set()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_set = true;
        }
        m_cv.notify_all();
    }

    bool Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::seconds(30), [this] { return m_set; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_set = false;
};

// Records what the cache asked its callbacks to do.
class Log
{
public:
    void Query(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queried.push_back(path);
    }

    void Notify(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_notified.push_back(path);
    }

    std::vector<std::wstring> Queried()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queried;
    }

    std::vector<std::wstring> Notified()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_notified;
    }

    size_t CountQueries(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = 0;
        for (const std::wstring& p : m_queried)
            if (p == path)
                ++count;
        return count;
    }

private:
    std::mutex m_mutex;
    std::vector<std::wstring> m_queried;
    std::vector<std::wstring> m_notified;
};

bool Equal(const std::vector<std::wstring>& actual, const std::vector<std::wstring>& expected)
{
    return actual == expected;
}

// ---------------------------------------------------------------------------
// Parser and helpers
// ---------------------------------------------------------------------------

void TestParsePorcelain()
{
    // Clean, with upstream in sync.
    CHECK(ParsePorcelainV2("# branch.oid abc\n# branch.head main\n# branch.upstream origin/main\n"
                           "# branch.ab +0 -0\n") == RepoStatus::Clean);
    // Clean, no upstream at all.
    CHECK(ParsePorcelainV2("# branch.oid abc\n# branch.head main\n") == RepoStatus::Clean);
    // Fresh repository with no commits.
    CHECK(ParsePorcelainV2("# branch.oid (initial)\n# branch.head main\n") == RepoStatus::Clean);
    CHECK(ParsePorcelainV2("") == RepoStatus::Clean);

    // Ahead / behind.
    CHECK(ParsePorcelainV2("# branch.head main\n# branch.ab +2 -0\n") == RepoStatus::AheadBehind);
    CHECK(ParsePorcelainV2("# branch.head main\n# branch.ab +0 -3\n") == RepoStatus::AheadBehind);
    CHECK(ParsePorcelainV2("# branch.head main\r\n# branch.ab +1 -1\r\n") == RepoStatus::AheadBehind);

    // Any working tree change wins over ahead/behind.
    CHECK(ParsePorcelainV2("# branch.ab +1 -0\n1 .M N... 100644 100644 100644 a b file.txt\n") ==
          RepoStatus::Modified);
    CHECK(ParsePorcelainV2("2 R. N... 100644 100644 100644 a b R100 new\told\n") == RepoStatus::Modified);
    CHECK(ParsePorcelainV2("u UU N... 100644 100644 100644 100644 a b c file\n") == RepoStatus::Modified);
    CHECK(ParsePorcelainV2("# branch.ab +0 -0\n? untracked.txt\n") == RepoStatus::Modified);

    // Ignored entries do not count as changes.
    CHECK(ParsePorcelainV2("# branch.ab +0 -0\n! build/\n") == RepoStatus::Clean);

    // Output cut off in the middle of a line (the reader stops at a size
    // limit): the first entry has already decided the badge.
    CHECK(ParsePorcelainV2("# branch.ab +0 -0\n1 .M N... 100644 100644 100644 a b very/lo") == RepoStatus::Modified);
}

void TestHasOverlay()
{
    CHECK(HasOverlay(RepoStatus::Clean));
    CHECK(HasOverlay(RepoStatus::Modified));
    CHECK(HasOverlay(RepoStatus::AheadBehind));
    CHECK(!HasOverlay(RepoStatus::Unknown));
    CHECK(!HasOverlay(RepoStatus::NotRepo));
    CHECK(!HasOverlay(RepoStatus::Error));
}

void TestRepositoryScopedConfig()
{
    CHECK(!HasRepositoryScopedEntries(""));
    CHECK(!HasRepositoryScopedEntries("global\tfilter.lfs.clean git-lfs clean -- %f\n"
                                      "system\tfilter.lfs.process git-lfs filter-process\n"
                                      "global\tinclude.path ~/.gitconfig-work\n"));
    CHECK(HasRepositoryScopedEntries("global\tfilter.lfs.clean git-lfs clean -- %f\n"
                                     "local\tfilter.evil.clean calc.exe\n"));
    CHECK(HasRepositoryScopedEntries("worktree\tfilter.x.process y\r\n"));
    CHECK(HasRepositoryScopedEntries("local\tinclude.path ../elsewhere\n"));
    CHECK(HasRepositoryScopedEntries("local\tlfs.extension.x.clean run-me %f\n"));
    CHECK(HasRepositoryScopedEntries("local\tcore.excludesfile //server/share/ignore\n"));
}

// The pattern is what keeps repository-controlled settings out, so check it
// against the key names git prints (section and variable lower-cased).
void TestUntrustedConfigPattern()
{
    const std::wregex pattern(kUntrustedConfigPattern);
    auto matches = [&](const wchar_t* key) { return std::regex_search(std::wstring(key), pattern); };

    CHECK(matches(L"filter.evil.clean"));
    CHECK(matches(L"filter.evil.process"));
    CHECK(matches(L"filter.a.b.clean")); // Subsection containing a dot.
    CHECK(matches(L"lfs.extension.foo.clean"));
    CHECK(matches(L"lfs.extension.foo.smudge"));
    CHECK(matches(L"include.path"));
    CHECK(matches(L"includeif.gitdir:c:/work/.path"));
    CHECK(matches(L"core.excludesfile"));
    CHECK(matches(L"core.attributesfile"));

    // Ordinary settings, including ones Git LFS and partial clones write into
    // every repository's own config, must not be refused.
    CHECK(!matches(L"filter.lfs.smudge"));
    CHECK(!matches(L"filter.lfs.required"));
    CHECK(!matches(L"lfs.repositoryformatversion"));
    CHECK(!matches(L"lfs.url"));
    CHECK(!matches(L"remote.origin.promisor"));
    CHECK(!matches(L"core.worktree"));
    CHECK(!matches(L"core.fsmonitor"));
    CHECK(!matches(L"core.excludesfilex"));
    CHECK(!matches(L"xinclude.path"));
    CHECK(!matches(L"user.name"));
}

void TestUncPaths()
{
    CHECK(IsUncPath(L"\\\\server\\share"));
    CHECK(IsUncPath(L"//server/share"));
    CHECK(IsUncPath(L"\\\\?\\C:\\repo"));
    CHECK(IsUncPath(L"\\\\.\\pipe\\x"));

    CHECK(!IsUncPath(L""));
    CHECK(!IsUncPath(L"C:\\"));
    CHECK(!IsUncPath(L"C:\\repo"));
    CHECK(!IsUncPath(L"\\repo"));
    CHECK(!IsUncPath(L"/repo"));
    CHECK(!IsUncPath(L"relative\\path"));
}

void TestUncGitdirPointer()
{
    CHECK(IsUncGitdirPointer("gitdir: //host/share/repo.git\n"));
    CHECK(IsUncGitdirPointer("gitdir: \\\\host@80\\share\\repo.git"));
    CHECK(IsUncGitdirPointer("gitdir: \\\\?\\UNC\\host\\share\\repo.git\r\n"));

    // Pointers that stay on the local machine, as written by git itself.
    CHECK(!IsUncGitdirPointer("gitdir: ../.git/modules/sub\n"));
    CHECK(!IsUncGitdirPointer("gitdir: C:/work/repo/.git/worktrees/wt\r\n"));
    CHECK(!IsUncGitdirPointer("gitdir: C:\\work\\repo\\.git\\worktrees\\wt\n"));
    // Not a gitdir pointer at all; git itself rejects these.
    CHECK(!IsUncGitdirPointer(""));
    CHECK(!IsUncGitdirPointer("gitdir:"));
    CHECK(!IsUncGitdirPointer("gitdir:\\\\host\\share"));
    CHECK(!IsUncGitdirPointer("\\\\host\\share"));
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

void TestCacheIsNonBlockingAndCaches()
{
    Log log;
    Event entered, release;

    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            entered.Set();
            release.Wait(); // Simulate a slow `git status`.
            return path == L"c:\\repo" ? RepoStatus::Modified : RepoStatus::NotRepo;
        },
        [&](const std::wstring& path) { log.Notify(path); }, std::chrono::hours(1));

    // The first lookup returns at once with "unknown", and so do later ones
    // while the query is stuck: nothing here waits for it.
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Unknown);
    CHECK(entered.Wait()); // The query is now running and blocked.
    for (int i = 0; i < 1000; ++i)
        CHECK(cache.Get(L"c:\\repo") == RepoStatus::Unknown);
    CHECK(cache.Get(L"c:\\plain") == RepoStatus::Unknown);

    release.Set();
    cache.WaitIdle();

    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Modified);
    CHECK(cache.Get(L"c:\\plain") == RepoStatus::NotRepo);
    // Each folder was checked exactly once despite repeated lookups.
    CHECK(log.CountQueries(L"c:\\repo") == 1);
    CHECK(log.CountQueries(L"c:\\plain") == 1);
    // Explorer is told to redraw the repository, but not plain folders.
    CHECK(Equal(log.Notified(), { L"c:\\repo" }));
}

void TestCacheKeyAndPathSpelling()
{
    Log log;
    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            return RepoStatus::Clean;
        },
        [&](const std::wstring& path) { log.Notify(path); }, std::chrono::hours(1));

    // Entries are found by key, but the callbacks get the path as first spelled.
    CHECK(cache.Get(L"c:\\repo", L"C:\\Repo") == RepoStatus::Unknown);
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\repo", L"c:\\REPO") == RepoStatus::Clean);
    cache.WaitIdle();
    CHECK(Equal(log.Queried(), { L"C:\\Repo" }));
    CHECK(Equal(log.Notified(), { L"C:\\Repo" }));
    CHECK(cache.Size() == 1);
}

void TestCacheRefreshesStaleEntriesInBackground()
{
    std::mutex m;
    RepoStatus current = RepoStatus::Clean;
    Log log;

    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            std::lock_guard<std::mutex> lock(m);
            return current;
        },
        [&](const std::wstring& path) { log.Notify(path); },
        std::chrono::milliseconds(0)); // Always stale.

    cache.Get(L"c:\\repo");
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Clean); // also queues a refresh
    cache.WaitIdle();
    CHECK(log.Notified().size() == 1); // unchanged status: no extra redraw

    {
        std::lock_guard<std::mutex> lock(m);
        current = RepoStatus::AheadBehind;
    }
    // Stale value is returned immediately; the refresh happens afterwards.
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Clean);
    cache.WaitIdle();
    CHECK(log.Notified().size() == 2);
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::AheadBehind);
    cache.WaitIdle();
    CHECK(log.CountQueries(L"c:\\repo") == 4);
}

void TestCachePicksUpNewRepository()
{
    // A plain folder that becomes a repository (the user runs `git init`).
    std::atomic<int> state{ 0 };
    Log log;
    StatusCache cache(
        [&](const std::wstring&) { return state == 0 ? RepoStatus::NotRepo : RepoStatus::Clean; },
        [&](const std::wstring& path) { log.Notify(path); }, std::chrono::milliseconds(0));

    cache.Get(L"c:\\new");
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\new") == RepoStatus::NotRepo);
    cache.WaitIdle();
    CHECK(log.Notified().empty());

    state = 1;
    CHECK(cache.Get(L"c:\\new") == RepoStatus::NotRepo); // Stale value, refresh queued.
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\new") == RepoStatus::Clean);
    cache.WaitIdle();
    CHECK(Equal(log.Notified(), { L"c:\\new" }));
}

void TestCacheRedrawsOnlyWhenTheBadgeChanges()
{
    // Scripted results: Clean, then Error (badge disappears), then NotRepo
    // (still no badge), then Clean again. The last entry is for the refresh
    // that the final Get() below queues.
    const RepoStatus script[] = { RepoStatus::Clean, RepoStatus::Error, RepoStatus::NotRepo, RepoStatus::Clean,
                                  RepoStatus::Clean };
    std::atomic<int> next{ 0 };
    Log log;
    StatusCache cache(
        [&](const std::wstring&) { return script[next++]; }, [&](const std::wstring& path) { log.Notify(path); },
        std::chrono::milliseconds(0));

    for (int i = 0; i < 4; ++i)
    {
        cache.Get(L"c:\\repo");
        cache.WaitIdle();
    }
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Clean); // The fourth result.
    cache.WaitIdle();
    // Unknown->Clean, Clean->Error and NotRepo->Clean redraw; Error->NotRepo
    // looks the same before and after, so it does not.
    CHECK(log.Notified().size() == 3);
}

void TestCacheQueryErrors()
{
    // A query that throws is reported as an error and the worker carries on.
    StatusCache thrower([](const std::wstring&) -> RepoStatus { throw 42; }, nullptr, std::chrono::hours(1));
    thrower.Get(L"c:\\broken");
    thrower.WaitIdle();
    CHECK(thrower.Get(L"c:\\broken") == RepoStatus::Error);

    // A query that reports an error is cached as one, without a redraw.
    Log log;
    StatusCache failing([](const std::wstring&) { return RepoStatus::Error; },
                        [&](const std::wstring& path) { log.Notify(path); }, std::chrono::hours(1));
    failing.Get(L"c:\\refused");
    failing.WaitIdle();
    CHECK(failing.Get(L"c:\\refused") == RepoStatus::Error);
    CHECK(log.Notified().empty());
}

void TestCacheSurvivesThrowingNotify()
{
    Log log;
    StatusCache cache(
        [](const std::wstring&) { return RepoStatus::Modified; },
        [&](const std::wstring& path) {
            log.Notify(path);
            throw 42;
        },
        std::chrono::hours(1));

    cache.Get(L"c:\\a");
    cache.Get(L"c:\\b");
    cache.WaitIdle();
    // Both were processed even though every notification threw.
    CHECK(cache.Get(L"c:\\a") == RepoStatus::Modified);
    CHECK(cache.Get(L"c:\\b") == RepoStatus::Modified);
    CHECK(log.Notified().size() == 2);
}

void TestCacheNewestRequestIsCheckedFirst()
{
    Log log;
    Event entered, release;
    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            if (path == L"c:\\first")
            {
                entered.Set();
                release.Wait();
            }
            return RepoStatus::Clean;
        },
        nullptr, std::chrono::hours(1));

    cache.Get(L"c:\\first");
    CHECK(entered.Wait()); // The worker is busy with "first".
    for (int i = 1; i <= 5; ++i)
        cache.Get(L"c:\\p" + std::to_wstring(i));
    release.Set();
    cache.WaitIdle();

    // What was asked about last (most likely still on screen) goes first.
    CHECK(Equal(log.Queried(), { L"c:\\first", L"c:\\p5", L"c:\\p4", L"c:\\p3", L"c:\\p2", L"c:\\p1" }));
}

void TestCacheTrim()
{
    Log log;
    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            return RepoStatus::Clean;
        },
        nullptr, std::chrono::hours(1), 3);

    for (const wchar_t* path : { L"c:\\a", L"c:\\b", L"c:\\c" })
    {
        cache.Get(path);
        cache.WaitIdle();
    }
    CHECK(cache.Size() == 3);
    CHECK(cache.Get(L"c:\\a") == RepoStatus::Clean); // Still cached.
    CHECK(log.Queried().size() == 3);

    // The limit is reached: everything not waiting for the worker is dropped.
    cache.Get(L"c:\\d");
    CHECK(cache.Size() == 1);
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\a") == RepoStatus::Unknown); // Evicted, checked again.
    cache.WaitIdle();
    CHECK(log.CountQueries(L"c:\\a") == 2);
}

void TestCacheTrimKeepsEntriesBeingChecked()
{
    Log log;
    Event entered, release;
    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            if (path == L"c:\\a")
            {
                entered.Set();
                release.Wait();
            }
            return RepoStatus::Modified;
        },
        [&](const std::wstring& path) { log.Notify(path); }, std::chrono::hours(1), 3);

    cache.Get(L"c:\\x");
    cache.Get(L"c:\\y");
    cache.WaitIdle();
    cache.Get(L"c:\\a"); // Now in flight, and the cache is at its limit.
    CHECK(entered.Wait());
    cache.Get(L"c:\\z"); // Forces a trim while "a" is being checked.
    CHECK(cache.Size() == 2); // x and y dropped; a (in flight) and z kept.

    release.Set();
    cache.WaitIdle();

    // The in-flight result was not lost, and was announced exactly once.
    CHECK(cache.Get(L"c:\\a") == RepoStatus::Modified);
    CHECK(log.CountQueries(L"c:\\a") == 1);
    CHECK(cache.Get(L"c:\\x") == RepoStatus::Unknown);
    size_t announcedA = 0;
    for (const std::wstring& path : log.Notified())
        if (path == L"c:\\a")
            ++announcedA;
    CHECK(announcedA == 1);
}

void TestCacheManyThreads()
{
    Log log;
    StatusCache cache(
        [&](const std::wstring& path) {
            log.Query(path);
            return RepoStatus::Clean;
        },
        nullptr, std::chrono::hours(1));

    std::vector<std::wstring> paths;
    for (int i = 0; i < 50; ++i)
        paths.push_back(L"c:\\p" + std::to_wstring(i));

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int round = 0; round < 200; ++round)
                for (const std::wstring& path : paths)
                    cache.Get(path);
        });
    for (std::thread& thread : threads)
        thread.join();
    cache.WaitIdle();

    // However many threads asked, each folder was checked exactly once.
    CHECK(log.Queried().size() == paths.size());
    for (const std::wstring& path : paths)
    {
        CHECK(log.CountQueries(path) == 1);
        CHECK(cache.Get(path) == RepoStatus::Clean);
    }
}

void TestCacheShutdownWithPendingWork()
{
    Log log;
    Event entered, release;
    std::atomic<bool> finished{ false };

    auto cache = std::make_unique<StatusCache>(
        [&](const std::wstring& path) {
            log.Query(path);
            entered.Set();
            release.Wait();
            finished = true;
            return RepoStatus::Clean;
        },
        nullptr, std::chrono::hours(1));

    for (int i = 0; i < 50; ++i)
        cache->Get(L"c:\\r" + std::to_wstring(i));
    CHECK(entered.Wait()); // One query is running; 49 more are queued.

    // Let the running query finish a little after destruction has begun.
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.Set();
    });
    cache.reset(); // Waits for the running query, drops the rest.
    opener.join();

    CHECK(finished); // The destructor waited for the query that was running.
    const size_t queriesAtShutdown = log.Queried().size();
    CHECK(queriesAtShutdown >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(log.Queried().size() == queriesAtShutdown); // Nothing runs afterwards.
}
} // namespace

int main()
{
    TestParsePorcelain();
    TestHasOverlay();
    TestRepositoryScopedConfig();
    TestUntrustedConfigPattern();
    TestUncPaths();
    TestUncGitdirPointer();
    TestCacheIsNonBlockingAndCaches();
    TestCacheKeyAndPathSpelling();
    TestCacheRefreshesStaleEntriesInBackground();
    TestCachePicksUpNewRepository();
    TestCacheRedrawsOnlyWhenTheBadgeChanges();
    TestCacheQueryErrors();
    TestCacheSurvivesThrowingNotify();
    TestCacheNewestRequestIsCheckedFirst();
    TestCacheTrim();
    TestCacheTrimKeepsEntriesBeingChecked();
    TestCacheManyThreads();
    TestCacheShutdownWithPendingWork();

    if (g_failures == 0)
        std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
