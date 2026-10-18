#include "Editor/ThumbnailAdmission.h"

#include <algorithm>

void ThumbnailAdmission::BeginFrame()
{
    for(auto& [key, entry] : m_entries)
    {
        entry.visible = false;
    }
}

ThumbnailAdmission::TileState ThumbnailAdmission::MarkVisible(const std::string& key, Clock::time_point now)
{
    auto [found, inserted] = m_entries.try_emplace(key);
    Entry& entry = found->second;

    if(inserted)
    {
        entry.waitingSince = now;
    }

    entry.visible = true;

    if(entry.admitted)
    {
        return TileState::Admitted;
    }

    return now < entry.retryAfter ? TileState::CoolingDown : TileState::Queued;
}

void ThumbnailAdmission::MarkShown(const std::string& key, Clock::time_point now)
{
    const auto found = m_entries.find(key);

    if(found != m_entries.end() && found->second.admitted && !found->second.shownSince)
    {
        found->second.shownSince = now;
    }
}

void ThumbnailAdmission::MarkFailed(const std::string& key, Clock::time_point now)
{
    const auto found = m_entries.find(key);

    if(found == m_entries.end() || !found->second.admitted)
    {
        return;
    }

    found->second.retryAfter = now + kFailureBackoff;
    m_failed.push_back(key);
}

void ThumbnailAdmission::Release(const std::string& key, Entry& entry, Clock::time_point requeueAt,
    std::vector<std::string>& released)
{
    entry.admitted = false;
    entry.shownSince.reset();
    entry.waitingSince = requeueAt;
    released.push_back(key);
}

void ThumbnailAdmission::Schedule(std::size_t budget, Clock::time_point now, std::vector<std::string>& released)
{
    // Failures leave admission now and rejoin the queue after their backoff.
    for(const auto& key : m_failed)
    {
        const auto found = m_entries.find(key);

        if(found != m_entries.end() && found->second.admitted)
        {
            Release(key, found->second, found->second.retryAfter, released);
        }
    }

    m_failed.clear();

    // Invisible tiles are forgotten; stuck generations time out like failures.
    for(auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if(!entry->second.visible)
        {
            if(entry->second.admitted)
            {
                released.push_back(entry->first);
            }

            entry = m_entries.erase(entry);
            continue;
        }

        const bool generationStuck = entry->second.admitted && !entry->second.shownSince &&
            now - entry->second.admittedAt >= kGenerationTimeout;

        if(generationStuck)
        {
            entry->second.retryAfter = now + kFailureBackoff;
            Release(entry->first, entry->second, entry->second.retryAfter, released);
        }

        ++entry;
    }

    // Waiting tiles in fair (oldest-first) order; ties break on the stable key.
    std::vector<std::map<std::string, Entry>::iterator> waiting;
    std::size_t admitted = 0;

    for(auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
    {
        if(entry->second.admitted)
        {
            ++admitted;
        }
        else if(now >= entry->second.retryAfter)
        {
            waiting.push_back(entry);
        }
    }

    std::stable_sort(waiting.begin(), waiting.end(), [](const auto& left, const auto& right)
    {
        return left->second.waitingSince < right->second.waitingSince;
    });

    // Rotation: while others wait and the budget is full, the tile shown longest yields once it
    // has been visible for the minimum residency. One per frame keeps churn bounded.
    if(!waiting.empty() && admitted >= budget)
    {
        auto yielding = m_entries.end();

        for(auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
        {
            const auto& candidate = entry->second;
            const bool resident = candidate.admitted && candidate.shownSince &&
                now - *candidate.shownSince >= kMinimumResidency;

            if(resident && (yielding == m_entries.end() || *candidate.shownSince < *yielding->second.shownSince))
            {
                yielding = entry;
            }
        }

        if(yielding != m_entries.end())
        {
            Release(yielding->first, yielding->second, now, released);
            --admitted;
        }
    }

    for(const auto& entry : waiting)
    {
        if(admitted >= budget)
        {
            break;
        }

        entry->second.admitted = true;
        entry->second.admittedAt = now;
        entry->second.shownSince.reset();
        ++admitted;
    }
}

void ThumbnailAdmission::Clear()
{
    m_entries.clear();
    m_failed.clear();
}

std::size_t ThumbnailAdmission::GetAdmittedCount() const
{
    return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(), [](const auto& entry)
    {
        return entry.second.admitted;
    }));
}

std::size_t ThumbnailAdmission::GetWaitingCount(Clock::time_point now) const
{
    return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(), [&](const auto& entry)
    {
        return !entry.second.admitted && now >= entry.second.retryAfter;
    }));
}
