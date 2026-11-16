#include "Debug/MemoryMonitor.h"

#include <Windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <mutex>

#pragma comment(lib, "dxgi.lib")

// Editor-only implementation; see MemoryMonitor.h for the build boundary.
//
// Threading: Start, Stop, IsSampleDue and Record run on the Editor's main thread; Snapshot may run anywhere and copies
// under the mutex. Lifetime: the state has static storage; its adapter pointer is raw and released in Stop, so nothing
// with a COM reference runs during static destruction (a monitor never stopped leaks one adapter reference at exit).

namespace
{
    struct MonitorState
    {
        std::mutex mutex;
        bool started = false;
        IDXGIAdapter3* adapter = nullptr;
        LUID adapterLuid{};
        std::chrono::steady_clock::time_point nextSample{};
        MemoryMonitorSnapshot latest;
    };

    MonitorState& State()
    {
        // Intentionally leaked: never destroyed, so Snapshot stays valid during static destruction.
        static MonitorState* state = new MonitorState;

        return *state;
    }

    ProcessMemoryStatistics QueryProcessMemory()
    {
        ProcessMemoryStatistics statistics;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);

        if(!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            return statistics;
        }

        statistics.valid = true;
        statistics.workingSetBytes = counters.WorkingSetSize;
        statistics.peakWorkingSetBytes = counters.PeakWorkingSetSize;
        statistics.privateBytes = counters.PrivateUsage;
        // Pagefile usage is the process's private commit charge; its peak is the peak private commit.
        statistics.peakPrivateBytes = counters.PeakPagefileUsage;

        return statistics;
    }

    // True when the failure means the adapter can no longer be queried (removal or reset).
    bool IsAdapterLost(HRESULT result)
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG;
    }

    VideoMemorySegment QuerySegment(IDXGIAdapter3* adapter, DXGI_MEMORY_SEGMENT_GROUP group, bool& lost)
    {
        VideoMemorySegment segment;
        DXGI_QUERY_VIDEO_MEMORY_INFO information{};
        const HRESULT result = adapter->QueryVideoMemoryInfo(0, group, &information);

        if(FAILED(result))
        {
            lost = lost || IsAdapterLost(result);
            return segment;
        }

        segment.valid = true;
        segment.usageBytes = information.CurrentUsage;
        segment.budgetBytes = information.Budget;
        segment.currentReservationBytes = information.CurrentReservation;
        segment.availableForReservationBytes = information.AvailableForReservation;

        return segment;
    }

    IDXGIAdapter3* FindAdapter(LUID luid)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;

        if(FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            return nullptr;
        }

        IDXGIAdapter3* adapter = nullptr;

        if(FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        {
            return nullptr;
        }

        return adapter;
    }
}

namespace MemoryMonitor
{
    void Start(ID3D12Device* device)
    {
        MonitorState& state = State();
        const std::lock_guard lock(state.mutex);

        if(state.started)
        {
            return;
        }

        state.started = true;
        state.nextSample = {};

        if(device != nullptr)
        {
            state.adapterLuid = device->GetAdapterLuid();
            state.adapter = FindAdapter(state.adapterLuid);
        }
    }

    void Stop()
    {
        MonitorState& state = State();
        const std::lock_guard lock(state.mutex);

        if(state.adapter != nullptr)
        {
            state.adapter->Release();
            state.adapter = nullptr;
        }

        state.started = false;
    }

    bool IsSampleDue(std::chrono::steady_clock::time_point now)
    {
        // Main thread only, like Start/Stop/Record, so these fields need no lock here.
        const MonitorState& state = State();

        return state.started && now >= state.nextSample;
    }

    void Record(std::chrono::steady_clock::time_point now, const CpuMemoryStatistics& cpu)
    {
        MonitorState& state = State();

        if(!state.started)
        {
            return;
        }

        // Queries happen outside the lock; only the copy into the published snapshot is guarded.
        MemoryMonitorSnapshot sample;
        sample.available = true;
        sample.sampleTime = now;
        sample.process = QueryProcessMemory();
        sample.gpuAllocations = MemoryRegistry::Snapshot();
        sample.cpu = cpu;

        if(state.adapter != nullptr)
        {
            bool lost = false;
            sample.video.adapterValid = true;
            sample.video.adapterLuidLow = state.adapterLuid.LowPart;
            sample.video.adapterLuidHigh = state.adapterLuid.HighPart;
            sample.video.local = QuerySegment(state.adapter, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, lost);
            sample.video.nonLocal = QuerySegment(state.adapter, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, lost);

            if(lost)
            {
                // A lost adapter is never queried again; the sample reports it invalid.
                sample.video = {};
                state.adapter->Release();
                state.adapter = nullptr;
            }
        }

        const std::lock_guard lock(state.mutex);
        sample.sequence = state.latest.sequence + 1;
        state.latest = sample;

        // From the due time, not from now, so the cadence does not drift with frame time. The next due time always lands
        // within one interval of now: a stall restarts the cadence instead of firing catch-up samples, and samples
        // recorded before they were due (direct Record calls) cannot push it further into the future.
        const auto candidate = state.nextSample + SampleInterval;
        const bool outsideWindow = state.nextSample == std::chrono::steady_clock::time_point{} || candidate <= now ||
            candidate > now + SampleInterval;

        if(outsideWindow)
        {
            state.nextSample = now + SampleInterval;
        }
        else
        {
            state.nextSample = candidate;
        }
    }

    MemoryMonitorSnapshot Snapshot()
    {
        MonitorState& state = State();
        const std::lock_guard lock(state.mutex);

        return state.latest;
    }
}
