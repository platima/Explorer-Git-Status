#include "StatusCache.h"

#include <utility>

StatusCache::StatusCache(QueryFn query, NotifyFn notify, Clock::duration ttl, size_t maxEntries)
    : m_query(std::move(query))
    , m_notify(std::move(notify))
    , m_ttl(ttl)
    , m_maxEntries(maxEntries)
{
}

StatusCache::~StatusCache()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

RepoStatus StatusCache::Get(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(path);
    if (it == m_entries.end())
    {
        TrimLocked();
        it = m_entries.emplace(path, Entry{}).first;
    }

    Entry& entry = it->second;
    const bool stale = !entry.hasResult || (Clock::now() - entry.checked) >= m_ttl;
    if (stale && !entry.queued && !m_stop)
    {
        entry.queued = true;
        m_queue.push_back(path);
        if (!m_worker.joinable())
            m_worker = std::thread(&StatusCache::WorkerLoop, this);
        m_wake.notify_one();
    }

    return entry.status;
}

void StatusCache::WaitIdle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_stop || (m_queue.empty() && !m_busy); });
}

void StatusCache::TrimLocked()
{
    if (m_entries.size() < m_maxEntries)
        return;

    // Drop everything that is not waiting for the worker; it will simply be
    // re-checked the next time Explorer asks for it.
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->second.queued)
            ++it;
        else
            it = m_entries.erase(it);
    }
}

void StatusCache::WorkerLoop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_stop)
            break;

        std::wstring path = std::move(m_queue.front());
        m_queue.pop_front();
        m_busy = true;

        lock.unlock();
        RepoStatus status = RepoStatus::Error;
        try
        {
            status = m_query(path);
        }
        catch (...)
        {
        }
        lock.lock();

        bool changed = false;
        auto it = m_entries.find(path);
        if (it != m_entries.end())
        {
            Entry& entry = it->second;
            // Plain folders never show a badge, so there is nothing to redraw
            // when an unchecked folder turns out not to be a repository.
            changed = entry.status != status &&
                      !(entry.status == RepoStatus::Unknown && status == RepoStatus::NotRepo);
            entry.status = status;
            entry.checked = Clock::now();
            entry.hasResult = true;
            entry.queued = false;
        }

        if (changed && m_notify)
        {
            lock.unlock();
            try
            {
                m_notify(path);
            }
            catch (...)
            {
            }
            lock.lock();
        }

        m_busy = false;
        if (m_queue.empty())
            m_idle.notify_all();
    }
    m_busy = false;
    m_idle.notify_all();
}
