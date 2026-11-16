#pragma once

#include "Debug/MemoryRegistry.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

struct ID3D12Device;

// M0b.1 P2 Editor-only memory monitor: process memory, DXGI video memory and Engine-attributed CPU statistics, sampled
// at a fixed interval from the Editor's main-thread frame. Same build boundary as MemoryRegistry.h: only the Editor links
// MemoryMonitor.cpp; every other EngineCore consumer resolves the empty functions in MemoryRegistryDisabled.cpp, where
// IsSampleDue is always false, so packaged games never gather statistics, query the OS or query DXGI for profiling.
//
// The three GPU figures are different measurements and are never reconciled:
//   gpuAllocations    Engine-registered D3D12 resources (P1), by allocation size;
//   video.local/...   what the OS reports this process uses on the adapter, per segment;
//   the difference    includes external libraries (NRD/NRI, DLSS), swap chains, driver and heap overhead, and is not
//                     reported as a figure.

// Process-wide (not Engine-attributed) memory from GetProcessMemoryInfo.
struct ProcessMemoryStatistics
{
    bool valid = false;
    // Physical memory currently mapped into the process (resident pages, shared ones included).
    std::uint64_t workingSetBytes = 0;
    std::uint64_t peakWorkingSetBytes = 0;
    // Committed private memory (PROCESS_MEMORY_COUNTERS_EX::PrivateUsage): what the process alone holds, resident or not.
    std::uint64_t privateBytes = 0;
    // Peak committed private memory (PeakPagefileUsage, which counts the same private commit).
    std::uint64_t peakPrivateBytes = 0;
};

// One DXGI memory segment group (IDXGIAdapter3::QueryVideoMemoryInfo) for this process.
struct VideoMemorySegment
{
    bool valid = false;
    std::uint64_t usageBytes = 0;
    std::uint64_t budgetBytes = 0;
    std::uint64_t currentReservationBytes = 0;
    std::uint64_t availableForReservationBytes = 0;
};

struct VideoMemoryStatistics
{
    // The adapter was found by the D3D12 device's LUID. False when no device was given, the lookup failed, or the device
    // was removed (sampling then stops querying DXGI).
    bool adapterValid = false;
    std::uint32_t adapterLuidLow = 0;
    std::int32_t adapterLuidHigh = 0;
    // Dedicated video memory on a discrete adapter; on an integrated (UMA) adapter, the shared system memory budget.
    VideoMemorySegment local;
    // System memory visible to the adapter on a discrete GPU; usually reports no budget on UMA adapters.
    VideoMemorySegment nonLocal;
};

// Existing Engine statistics only. Each entry is disjoint from the others (different allocations and allocators), but
// none is complete, so there is deliberately no CPU total.
enum class CpuMemoryStatistic : std::uint8_t
{
    // ProjectMeshCPUCache: geometry + metadata payload bytes of cached entries (vector sizes, not allocator overhead).
    MeshCacheRetained,
    // ProjectMeshCPUCache: geometry payload bytes of live leases that are no longer (or never were) cached.
    MeshLeasesOutsideCache,
    // ProjectTexturePreparationService: its byte budget in use, including reservations for in-flight preparation and
    // consumer-held leases; an accounted figure, not measured allocations.
    TexturePreparationRetained,
    // FMOD's allocator (FMOD::Memory_GetStats): exact bytes FMOD currently holds. Invalid when FMOD is not loaded.
    AudioBackend,
    Count
};

inline constexpr std::size_t CpuMemoryStatisticCount = static_cast<std::size_t>(CpuMemoryStatistic::Count);

enum class CpuMemoryStatisticKind : std::uint8_t
{
    // Counted by the owning allocator.
    Exact,
    // The subsystem's own accounting of payload or reserved bytes.
    Accounted
};

struct CpuMemoryStatisticValue
{
    bool valid = false;
    std::uint64_t bytes = 0;
};

using CpuMemoryStatistics = std::array<CpuMemoryStatisticValue, CpuMemoryStatisticCount>;

struct MemoryMonitorSnapshot
{
    // False when no sample has been recorded (or in builds with the disabled monitor).
    bool available = false;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point sampleTime{};
    ProcessMemoryStatistics process;
    VideoMemoryStatistics video;
    MemoryRegistrySnapshot gpuAllocations;
    CpuMemoryStatistics cpu{};
};

namespace MemoryMonitor
{
    inline constexpr std::chrono::milliseconds SampleInterval{500};

    // Main thread. Resolves the DXGI adapter from the device's LUID and keeps one adapter reference until Stop.
    void Start(ID3D12Device* device);
    // Main thread, before the device is destroyed. Drops the adapter; nothing is sampled afterwards.
    void Stop();

    // Main thread, once per frame: cheap clock comparison. True when started and SampleInterval has passed.
    bool IsSampleDue(std::chrono::steady_clock::time_point now);
    // Main thread: queries process memory and DXGI, copies the registry snapshot and the given CPU statistics.
    void Record(std::chrono::steady_clock::time_point now, const CpuMemoryStatistics& cpu);

    // A copy of the latest sample; safe from any thread.
    MemoryMonitorSnapshot Snapshot();

    constexpr const char* CpuStatisticName(CpuMemoryStatistic statistic)
    {
        switch(statistic)
        {
            case CpuMemoryStatistic::MeshCacheRetained:
                return "Mesh CPU cache";
            case CpuMemoryStatistic::MeshLeasesOutsideCache:
                return "Mesh leases outside cache";
            case CpuMemoryStatistic::TexturePreparationRetained:
                return "Texture preparation";
            case CpuMemoryStatistic::AudioBackend:
                return "Audio (FMOD)";
            case CpuMemoryStatistic::Count:
                break;
        }

        return "Unknown";
    }

    constexpr CpuMemoryStatisticKind CpuStatisticKind(CpuMemoryStatistic statistic)
    {
        return statistic == CpuMemoryStatistic::AudioBackend ? CpuMemoryStatisticKind::Exact : CpuMemoryStatisticKind::Accounted;
    }
}
