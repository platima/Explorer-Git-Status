#pragma once

#include "GitStatus.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Non-blocking cache of folder git statuses.
//
// Get() never performs any I/O: it returns whatever is cached (Unknown if the
// folder has never been seen) and, if the entry is missing or older than the
// time-to-live, queues it for a refresh on a background worker thread. When
// the worker finds a status different from the cached one it invokes the
// notify callback so the shell can redraw the item.
//
// This keeps Explorer responsive: the first time a folder is displayed it has
// no badge, and the badge appears as soon as the background check completes.
class StatusCache
{
public:
    using QueryFn = std::function<RepoStatus(const std::wstring& path)>;
    using NotifyFn = std::function<void(const std::wstring& path)>;
    using Clock = std::chrono::steady_clock;

    StatusCache(QueryFn query, NotifyFn notify, Clock::duration ttl, size_t maxEntries = 20000);
    ~StatusCache();

    StatusCache(const StatusCache&) = delete;
    StatusCache& operator=(const StatusCache&) = delete;

    // Returns the cached status for `path` without blocking.
    RepoStatus Get(const std::wstring& path);

    // Blocks until the work queue is empty (used by tests).
    void WaitIdle();

private:
    struct Entry
    {
        RepoStatus status = RepoStatus::Unknown;
        Clock::time_point checked{};
        bool hasResult = false;
        bool queued = false;
    };

    void WorkerLoop();
    void TrimLocked();

    QueryFn m_query;
    NotifyFn m_notify;
    Clock::duration m_ttl;
    size_t m_maxEntries;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::unordered_map<std::wstring, Entry> m_entries;
    std::deque<std::wstring> m_queue;
    bool m_busy = false;
    bool m_stop = false;
    std::thread m_worker;
};
