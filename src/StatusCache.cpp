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
    return Get(path, path);
}

RepoStatus StatusCache::Get(const std::wstring& key, const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(key);
    if (it == m_entries.end())
    {
        TrimLocked();
        it = m_entries.emplace(key, Entry{}).first;
        it->second.path = path;
    }

    Entry& entry = it->second;
    const bool stale = !entry.hasResult || (Clock::now() - entry.checked) >= m_ttl;
    if (stale && !entry.queued && !m_stop)
    {
        // Everything that can throw (starting the thread, growing the queue)
        // comes before the entry is marked, so a failure leaves it untouched
        // and it is simply tried again on the next lookup.
        if (!m_worker.joinable())
            m_worker = std::thread(&StatusCache::WorkerLoop, this);
        m_queue.push_back(key);
        entry.queued = true;
        m_wake.notify_one();
    }

    return entry.status;
}

void StatusCache::WaitIdle()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_stop || (m_queue.empty() && !m_busy); });
}

size_t StatusCache::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
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

        // Newest first: the folder Explorer asked about last is the one most
        // likely still on screen, so it should not wait behind folders the user
        // has already scrolled or navigated away from.
        std::wstring key = std::move(m_queue.back());
        m_queue.pop_back();

        auto it = m_entries.find(key);
        if (it == m_entries.end())
        {
            // Not expected: queued entries are never trimmed.
            if (m_queue.empty())
                m_idle.notify_all();
            continue;
        }
        const std::wstring path = it->second.path;
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
        it = m_entries.find(key);
        if (it != m_entries.end())
        {
            Entry& entry = it->second;
            // Only a change in what is drawn needs a redraw: a plain folder
            // turning out not to be a repository, or one error turning into
            // another, looks the same before and after.
            changed = entry.status != status && (HasOverlay(entry.status) || HasOverlay(status));
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
