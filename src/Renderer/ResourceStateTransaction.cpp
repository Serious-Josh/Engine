#include "ResourceStateTransaction.h"

#include <algorithm>
#include <vector>

namespace
{
    std::uint64_t g_nextRecording = 1;
    std::uint64_t g_openRecording = 0;
    const void* g_openList = nullptr;
    const void* g_openRayTracingList = nullptr;
    // Ascending (recording numbers only grow).
    std::vector<std::uint64_t> g_discarded;
    ResourceStateTransaction::Statistics g_statistics;
    bool g_diagnostics = false;
    std::vector<const void*> g_openResources;
    std::vector<const void*> g_discardedResources;
}

namespace ResourceStateTransaction
{
    void BeginRecording(const void* commandList, const void* rayTracingCommandList)
    {
        if(g_openRecording != 0)
        {
            DiscardRecording();
        }

        g_openRecording = g_nextRecording++;
        g_openResources.clear();
        g_openList = commandList;
        g_openRayTracingList = rayTracingCommandList;
        ++g_statistics.recordings;
    }

    void CommitRecording()
    {
        if(g_openRecording == 0)
        {
            return;
        }

        g_openRecording = 0;
        g_openList = nullptr;
        g_openRayTracingList = nullptr;
        ++g_statistics.committed;
    }

    void DiscardRecording()
    {
        if(g_openRecording == 0)
        {
            return;
        }

        g_discarded.push_back(g_openRecording);
        g_discardedResources.swap(g_openResources);
        g_openResources.clear();
        g_openRecording = 0;
        g_openList = nullptr;
        g_openRayTracingList = nullptr;
        ++g_statistics.discarded;
    }

    std::uint64_t RecordingFor(const void* commandList)
    {
        return g_openRecording != 0 && commandList != nullptr &&
            (commandList == g_openList || commandList == g_openRayTracingList) ? g_openRecording : 0;
    }

    bool IsDiscarded(std::uint64_t recording)
    {
        return !g_discarded.empty() && recording != g_openRecording &&
            std::binary_search(g_discarded.begin(), g_discarded.end(), recording);
    }

    Statistics GetStatistics()
    {
        return g_statistics;
    }

    void CountRestored()
    {
        ++g_statistics.restored;
    }

    void SetDiagnostics(bool enabled)
    {
        g_diagnostics = enabled;
    }

    bool DiagnosticsEnabled()
    {
        return g_diagnostics;
    }

    void NoteFirstChange(const void* resource)
    {
        g_openResources.push_back(resource);
    }

    const std::vector<const void*>& LastDiscardedResources()
    {
        return g_discardedResources;
    }
}
