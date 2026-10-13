#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <vector>

// Bounded retirement through slot ownership: obsolete sets leave only at checked
// slot acquisition. No separate growing retirement queue and no GPU waits here.
template<class Handle, size_t SlotCount, size_t Capacity>
class MaterialDependencyRetention
{
public:
    struct Slot
    {
        std::vector<Handle> dependencies;

        std::uint64_t revision = 0;
        std::uint64_t fence = 0;

        bool recorded = false;
        bool uncertain = false;
    };
    ~MaterialDependencyRetention()
    {
        // Releasing unresolved leases during implicit Renderer destruction is unsafe.
        if(std::any_of(m_slots.begin(), m_slots.end(),
            [](const Slot& slot) { return !slot.dependencies.empty(); }))
        {
            std::terminate();
        }
    }

    bool SetDesired(std::vector<Handle> dependencies)
    {
        std::vector<Handle> unique;

        for(auto& dependency : dependencies)
        {
            if(!dependency)
            {
                return false;
            }

            if(std::find(unique.begin(), unique.end(), dependency) == unique.end())
            {
                unique.push_back(std::move(dependency));
            }
        }

        auto retained = unique;

        for(const auto& slot : m_slots)
        {
            for(const auto& dependency : slot.dependencies)
            {
                if(std::find(retained.begin(), retained.end(), dependency) == retained.end())
                {
                    retained.push_back(dependency);
                }
            }
        }

        if(retained.size() > Capacity)
        {
            return false; // Future updates defer; never release a slot's in-flight set.
        }

        m_desired.swap(unique);
        return true;
    }

    const std::vector<Handle>& Desired() const noexcept
    {
        return m_desired;
    }

    const Slot& GetSlot(size_t slot) const
    {
        return m_slots.at(slot);
    }

    bool Acquire(size_t index, std::uint64_t completedFence, std::uint64_t revision)
    {
        if(index >= SlotCount || completedFence == UINT64_MAX || revision == 0)
        {
            return false;
        }

        auto& slot = m_slots[index];

        if(slot.recorded || slot.uncertain || slot.fence > completedFence)
        {
            return false;
        }

        if(slot.dependencies != m_desired)
        {
            auto dependencies = m_desired; // Allocate before dropping the old, now completed set.
            slot.dependencies.swap(dependencies);
        }

        slot.revision = revision;
        slot.recorded = true;
        return true;
    }

    bool Confirm(size_t index, std::uint64_t fence)
    {
        if(index >= SlotCount || fence == 0 || fence == UINT64_MAX || !m_slots[index].recorded || m_slots[index].uncertain ||
           fence < m_slots[index].fence)
        {
            return false;
        }

        auto& slot = m_slots[index];
        slot.fence = fence;
        slot.recorded = false;
        return true;
    }

    void Discard(size_t index) noexcept
    {
        if(index < SlotCount && !m_slots[index].uncertain)
        {
            m_slots[index].recorded = false;
            // Conservative: retain the discarded set until checked reuse or teardown.
        }
    }

    void MarkUncertain(size_t index) noexcept
    {
        if(index < SlotCount)
        {
            m_slots[index].uncertain = true;
        }
    }

    // Caller has proved GPU idle AND discarded every unsubmitted command list.
    void ClearAfterGPUDrain() noexcept
    {
        m_desired.clear();

        for(auto& slot : m_slots)
        {
            slot = {};
        }
    }

private:
    std::vector<Handle> m_desired;

    std::array<Slot, SlotCount> m_slots{};
};
