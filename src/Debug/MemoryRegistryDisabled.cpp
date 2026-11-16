#include "Debug/MemoryRegistry.h"

// Linked by every EngineCore consumer except the Editor, whose MemoryRegistry.cpp object takes precedence over this
// library member. Keep this file's function set identical to MemoryRegistry.cpp: the linker then never loads this
// member into the Editor, and never loads MemoryRegistry.cpp into anything else.

namespace MemoryRegistry
{
    void TrackResource(ID3D12Resource*, MemoryCategory)
    {
    }

    MemoryRegistrySnapshot Snapshot()
    {
        return {};
    }

    bool IsTracked(ID3D12Resource*)
    {
        return false;
    }

    std::optional<MemoryResourceRecord> FindTrackedResource(ID3D12Resource*)
    {
        return std::nullopt;
    }

    MemoryCategory PushAttribution(MemoryCategory)
    {
        return MemoryCategory::Unattributed;
    }

    void PopAttribution(MemoryCategory)
    {
    }
}
