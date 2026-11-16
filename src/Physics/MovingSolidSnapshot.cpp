#include "Physics/MovingSolidSnapshot.h"

#include <algorithm>

namespace Physics
{
std::span<const MovingSolidEntry> MovingSolidSnapshot::GetEntries() const noexcept
{
    return m_entries;
}

const MovingSolidEntry* MovingSolidSnapshot::Find(std::string_view objectID) const noexcept
{
    const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), objectID,
        [](const MovingSolidEntry& entry, std::string_view requestedID)
        {
            return entry.objectID < requestedID;
        });

    if(found == m_entries.end() || found->objectID != objectID)
    {
        return nullptr;
    }

    return &*found;
}

std::uint64_t MovingSolidSnapshot::GetSessionGeneration() const noexcept
{
    return m_sessionGeneration;
}

std::uint64_t MovingSolidSnapshot::GetCompletedSteps() const noexcept
{
    return m_completedSteps;
}
}
