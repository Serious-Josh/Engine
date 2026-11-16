#include "Debug/MemoryMonitor.h"

// Separate library member from MemoryRegistryDisabled.cpp; keep its function set identical to MemoryMonitor.cpp.

// IsSampleDue is always false, so callers never gather statistics.
namespace MemoryMonitor
{
    void Start(ID3D12Device*)
    {
    }

    void Stop()
    {
    }

    bool IsSampleDue(std::chrono::steady_clock::time_point)
    {
        return false;
    }

    void Record(std::chrono::steady_clock::time_point, const CpuMemoryStatistics&)
    {
    }

    MemoryMonitorSnapshot Snapshot()
    {
        return {};
    }
}
