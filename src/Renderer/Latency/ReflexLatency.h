#pragma once

#include "Renderer/Latency/ReflexStatus.h"

#include <cstdint>

struct ID3D12Device;

// NVIDIA Reflex Low Latency through NVAPI's D3D sleep mode, sleep and latency markers. Every NVAPI call in Engine goes
// through this class. Not thread-safe: the render thread owns it, and the frame loop calls it in this order:
//
//   BeginFrame (NvAPI_D3D_Sleep, then SIMULATION_START)  ->  MarkInputSampled (INPUT_SAMPLE)
//   ->  MarkRenderSubmitStart (SIMULATION_END, RENDERSUBMIT_START)  ->  MarkPresentStart (RENDERSUBMIT_END, PRESENT_START)
//   ->  Present  ->  MarkPresentEnd (PRESENT_END)
//
// A frame that returns early simply sends fewer markers; frame IDs still increase strictly, as NVAPI requires.
//
// With ReflexMode::Off no NVAPI call is made at all (after a single SetSleepMode that turns an earlier mode off), so the
// frame loop behaves exactly as it did before Reflex existed; only the ENGINE_REFLEX_MEASURE_OFF=1 diagnostics send markers
// then, for an Off/On latency comparison. Sleep mode belongs to the D3D12 device and ends with it, so
// shutdown needs no NVAPI call; nothing here is used after the renderer releases the device.
class ReflexLatency
{
public:
    ReflexLatency() = default;
    ReflexLatency(const ReflexLatency&) = delete;
    ReflexLatency& operator=(const ReflexLatency&) = delete;

    // Applies mode (initializing NVAPI on the first request that is not Off), then, while Reflex is active, sleeps as the
    // driver asks and opens the frame with SIMULATION_START. device is the renderer's device; null leaves Reflex inactive.
    void BeginFrame(ID3D12Device* device, ReflexMode mode);

    void MarkInputSampled();
    void MarkRenderSubmitStart();
    void MarkPresentStart();
    void MarkPresentEnd();

    const ReflexStatus& GetStatus() const noexcept
    {
        return m_status;
    }

private:
    // Whether markers belong to the open frame: Reflex is available and BeginFrame opened a frame (applied or measured).
    bool IsTracking() const noexcept;

    void Initialize(ID3D12Device* device);
    bool ApplyMode(ReflexMode mode);
    void SetMarker(int markerType);
    void RefreshLatencyReport();

    ID3D12Device* m_device = nullptr;
    bool m_initializationAttempted = false;
    bool m_frameOpen = false;
    // The last mode NvAPI_D3D_SetSleepMode refused; not requested again until the frames ask for another mode.
    bool m_modeRefused = false;
    ReflexMode m_refusedMode = ReflexMode::Off;
    // Strictly increasing while Reflex runs (NvAPI_D3D_SetLatencyMarker requires it); never reset.
    std::uint64_t m_frameID = 0;
    std::uint32_t m_framesSinceLatencyReport = 0;
    ReflexStatus m_status;
};
