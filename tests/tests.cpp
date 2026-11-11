// Minimal self-contained tests for the portable core (no framework needed).

#include "GitStatus.h"
#include "StatusCache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
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

static void TestParsePorcelain()
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
}

static void TestRepositoryScopedConfig()
{
    CHECK(!HasRepositoryScopedEntries(""));
    CHECK(!HasRepositoryScopedEntries("global\tfilter.lfs.clean git-lfs clean -- %f\n"
                                      "system\tfilter.lfs.process git-lfs filter-process\n"));
    CHECK(HasRepositoryScopedEntries("global\tfilter.lfs.clean git-lfs clean -- %f\n"
                                     "local\tfilter.evil.clean calc.exe\n"));
    CHECK(HasRepositoryScopedEntries("worktree\tfilter.x.process y\r\n"));
}

// Waits (bounded) for a condition to become true.
template <typename F>
static bool WaitFor(F condition)
{
    for (int i = 0; i < 500; ++i)
    {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

static void TestCacheIsNonBlockingAndCaches()
{
    std::atomic<int> queries{ 0 };
    std::atomic<bool> release{ false };
    std::mutex notifiedMutex;
    std::vector<std::wstring> notified;

    StatusCache cache(
        [&](const std::wstring& path) {
            ++queries;
            // Simulate a slow `git status`.
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return path == L"c:\\repo" ? RepoStatus::Modified : RepoStatus::NotRepo;
        },
        [&](const std::wstring& path) {
            std::lock_guard<std::mutex> lock(notifiedMutex);
            notified.push_back(path);
        },
        std::chrono::hours(1));

    // First lookup returns immediately even though the query is blocked.
    auto start = std::chrono::steady_clock::now();
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Unknown);
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Unknown);
    CHECK(cache.Get(L"c:\\plain") == RepoStatus::Unknown);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));

    release = true;
    cache.WaitIdle();

    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Modified);
    CHECK(cache.Get(L"c:\\plain") == RepoStatus::NotRepo);
    // Each folder was checked exactly once despite repeated lookups.
    CHECK(queries == 2);

    // Explorer is told to redraw the repository, but not plain folders.
    std::lock_guard<std::mutex> lock(notifiedMutex);
    CHECK(notified.size() == 1);
    CHECK(!notified.empty() && notified[0] == L"c:\\repo");
}

static void TestCacheRefreshesStaleEntriesInBackground()
{
    std::mutex m;
    RepoStatus current = RepoStatus::Clean;
    std::atomic<int> queries{ 0 };
    std::atomic<int> notifications{ 0 };

    StatusCache cache(
        [&](const std::wstring&) {
            ++queries;
            std::lock_guard<std::mutex> lock(m);
            return current;
        },
        [&](const std::wstring&) { ++notifications; },
        std::chrono::milliseconds(0)); // Always stale.

    cache.Get(L"c:\\repo");
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Clean); // also queues a refresh
    cache.WaitIdle();
    CHECK(notifications == 1); // unchanged status: no extra redraw

    {
        std::lock_guard<std::mutex> lock(m);
        current = RepoStatus::AheadBehind;
    }
    // Stale value is returned immediately; the refresh happens afterwards.
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::Clean);
    cache.WaitIdle();
    CHECK(WaitFor([&] { return notifications == 2; }));
    CHECK(cache.Get(L"c:\\repo") == RepoStatus::AheadBehind);
    cache.WaitIdle();
    CHECK(queries >= 4);
}

static void TestCacheQueryErrors()
{
    StatusCache cache([](const std::wstring&) -> RepoStatus { throw 42; }, nullptr, std::chrono::hours(1));
    cache.Get(L"c:\\broken");
    cache.WaitIdle();
    CHECK(cache.Get(L"c:\\broken") == RepoStatus::Error);
}

static void TestCacheTrim()
{
    StatusCache cache([](const std::wstring&) { return RepoStatus::NotRepo; }, nullptr, std::chrono::hours(1), 10);
    for (int i = 0; i < 100; ++i)
    {
        cache.Get(L"c:\\dir" + std::to_wstring(i));
        cache.WaitIdle();
    }
    // Still functional after trimming.
    CHECK(cache.Get(L"c:\\dir99") == RepoStatus::NotRepo);
}

static void TestCacheShutdownWithPendingWork()
{
    std::atomic<int> queries{ 0 };
    {
        StatusCache cache(
            [&](const std::wstring&) {
                ++queries;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return RepoStatus::Clean;
            },
            nullptr, std::chrono::hours(1));
        for (int i = 0; i < 50; ++i)
            cache.Get(L"c:\\r" + std::to_wstring(i));
    } // Destructor must not wait for the whole queue.
    CHECK(queries < 50);
}

int main()
{
    TestParsePorcelain();
    TestRepositoryScopedConfig();
    TestCacheIsNonBlockingAndCaches();
    TestCacheRefreshesStaleEntriesInBackground();
    TestCacheQueryErrors();
    TestCacheTrim();
    TestCacheShutdownWithPendingWork();

    if (g_failures == 0)
        std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
