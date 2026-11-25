#include "Renderer/Latency/ReflexLatency.h"

#include "Core/Log.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#if defined(ENGINE_HAS_NVAPI)
// nvapi.h declares its D3D12 entry points only when d3d12.h came first, and its swap-chain entry points need DXGI.
#include <d3d12.h>
#include <dxgi.h>
#include <nvapi.h>
#endif

namespace
{
    // NvAPI_D3D_GetLatency copies 64 frame reports; reading them every 60 frames keeps the readout current at a negligible
    // cost.
    constexpr std::uint32_t kLatencyReportInterval = 60;

    // Diagnostics: ENGINE_REFLEX_MEASURE_OFF=1 sends the latency markers (never the sleep or a sleep mode) while Reflex is
    // Off, so the latency readout can compare Off with On. Without it, Off makes no NVAPI call.
    bool MeasureWhileOff()
    {
        static const bool requested = []
        {
            const char* value = std::getenv("ENGINE_REFLEX_MEASURE_OFF");
            return value && std::string(value) == "1";
        }();

        return requested;
    }

    // Diagnostics: ENGINE_REFLEX_LOG_LATENCY=1 logs every latency report (about once a second at 60 fps).
    bool LogLatencyReports()
    {
        static const bool requested = []
        {
            const char* value = std::getenv("ENGINE_REFLEX_LOG_LATENCY");
            return value && std::string(value) == "1";
        }();

        return requested;
    }

#if defined(ENGINE_HAS_NVAPI)
    std::string NvapiMessage(NvAPI_Status result)
    {
        NvAPI_ShortString text{};
        NvAPI_GetErrorMessage(result, text);
        return std::string(text) + " (" + std::to_string(static_cast<int>(result)) + ")";
    }
#endif
}

const char* ReflexModeName(ReflexMode mode) noexcept
{
    switch(mode)
    {
    case ReflexMode::On:
        return "On";
    case ReflexMode::OnBoost:
        return "On + Boost";
    case ReflexMode::Off:
    default:
        return "Off";
    }
}

const char* ReflexAvailabilityName(ReflexAvailability availability) noexcept
{
    switch(availability)
    {
    case ReflexAvailability::NotCompiled:
        return "Not compiled (built without the NVAPI SDK)";
    case ReflexAvailability::NotInitialized:
        return "Not initialized (not requested yet)";
    case ReflexAvailability::NvapiUnavailable:
        return "Unavailable (no NVIDIA driver)";
    case ReflexAvailability::Unsupported:
        return "Unsupported on this GPU or driver";
    case ReflexAvailability::Available:
    default:
        return "Available";
    }
}

std::string DescribeReflexStatus(const ReflexStatus& status)
{
    std::string text;

    if(status.availability == ReflexAvailability::Available)
    {
        text = std::string("requested ") + ReflexModeName(status.requested) + ", applied " + ReflexModeName(status.applied) +
            (status.driverLowLatencyMode ? " (driver low latency on)" : " (driver low latency off)");
    }
    else
    {
        text = ReflexAvailabilityName(status.availability);
    }

    if(!status.detail.empty())
    {
        text += "; " + status.detail;
    }

    return text;
}

void ReflexLatency::BeginFrame(ID3D12Device* device, ReflexMode mode)
{
    m_status.requested = mode;
    m_status.lastSleepMilliseconds = 0.0;
    m_frameOpen = false;

#if !defined(ENGINE_HAS_NVAPI)
    (void)device;
    m_status.availability = ReflexAvailability::NotCompiled;
#else
    if(!device)
    {
        return;
    }

    // NVAPI stays uninitialized until a frame asks for Reflex, so a project that never enables it makes no NVAPI call.
    if(!m_initializationAttempted || device != m_device)
    {
        if(mode == ReflexMode::Off && !MeasureWhileOff())
        {
            return;
        }

        Initialize(device);
    }

    if(m_status.availability != ReflexAvailability::Available)
    {
        return;
    }

    // A refused mode is not retried until the request changes; the previously applied mode stays in effect meanwhile.
    if(mode != m_status.applied)
    {
        const bool refusedBefore = m_modeRefused && mode == m_refusedMode;

        if(!refusedBefore)
        {
            ApplyMode(mode);
        }
    }

    const bool lowLatencyActive = m_status.applied != ReflexMode::Off;

    if(!lowLatencyActive && !MeasureWhileOff())
    {
        return;
    }

    // The driver's sleep point: as early in the frame as possible, before input is sampled.
    if(lowLatencyActive)
    {
        const auto sleepStart = std::chrono::steady_clock::now();
        const NvAPI_Status sleepResult = NvAPI_D3D_Sleep(m_device);
        m_status.lastSleepMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sleepStart).count();

        if(sleepResult != NVAPI_OK && m_status.detail.empty())
        {
            m_status.detail = "NvAPI_D3D_Sleep failed: " + NvapiMessage(sleepResult);
            Log::Warning("NVIDIA Reflex: " + m_status.detail);
        }
    }

    ++m_frameID;
    m_frameOpen = true;
    SetMarker(SIMULATION_START);

    ++m_framesSinceLatencyReport;

    if(m_framesSinceLatencyReport >= kLatencyReportInterval)
    {
        m_framesSinceLatencyReport = 0;
        RefreshLatencyReport();
    }
#endif
}

void ReflexLatency::MarkInputSampled()
{
#if defined(ENGINE_HAS_NVAPI)
    if(IsTracking())
    {
        SetMarker(INPUT_SAMPLE);
    }
#endif
}

void ReflexLatency::MarkRenderSubmitStart()
{
#if defined(ENGINE_HAS_NVAPI)
    if(IsTracking())
    {
        SetMarker(SIMULATION_END);
        SetMarker(RENDERSUBMIT_START);
    }
#endif
}

void ReflexLatency::MarkPresentStart()
{
#if defined(ENGINE_HAS_NVAPI)
    if(IsTracking())
    {
        SetMarker(RENDERSUBMIT_END);
        SetMarker(PRESENT_START);
    }
#endif
}

void ReflexLatency::MarkPresentEnd()
{
#if defined(ENGINE_HAS_NVAPI)
    if(IsTracking())
    {
        SetMarker(PRESENT_END);
    }
#endif

    m_frameOpen = false;
}

bool ReflexLatency::IsTracking() const noexcept
{
    // BeginFrame opens a frame only while Reflex is available and either applied or measured.
    return m_frameOpen && m_status.availability == ReflexAvailability::Available;
}

void ReflexLatency::Initialize(ID3D12Device* device)
{
#if defined(ENGINE_HAS_NVAPI)
    m_initializationAttempted = true;
    m_device = device;
    // A new device starts without a sleep mode of its own.
    m_status.applied = ReflexMode::Off;
    m_status.driverLowLatencyMode = false;
    m_status.detail.clear();

    const NvAPI_Status initializeResult = NvAPI_Initialize();

    if(initializeResult != NVAPI_OK)
    {
        m_status.availability = ReflexAvailability::NvapiUnavailable;
        m_status.detail = "NvAPI_Initialize failed: " + NvapiMessage(initializeResult);
        Log::Info("NVIDIA Reflex unavailable: " + m_status.detail);
        return;
    }

    // GetSleepStatus succeeds only where the driver supports Reflex on this device.
    NV_GET_SLEEP_STATUS_PARAMS sleepStatus{};
    sleepStatus.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
    const NvAPI_Status statusResult = NvAPI_D3D_GetSleepStatus(m_device, &sleepStatus);

    if(statusResult != NVAPI_OK)
    {
        m_status.availability = ReflexAvailability::Unsupported;
        m_status.detail = "NvAPI_D3D_GetSleepStatus failed: " + NvapiMessage(statusResult);
        Log::Info("NVIDIA Reflex unsupported: " + m_status.detail);
        return;
    }

    m_status.availability = ReflexAvailability::Available;
    m_status.driverLowLatencyMode = sleepStatus.bLowLatencyMode != 0;
    Log::Info("NVIDIA Reflex available.");
#else
    (void)device;
#endif
}

bool ReflexLatency::ApplyMode(ReflexMode mode)
{
#if defined(ENGINE_HAS_NVAPI)
    NV_SET_SLEEP_MODE_PARAMS sleepMode{};
    sleepMode.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    sleepMode.bLowLatencyMode = mode != ReflexMode::Off;
    sleepMode.bLowLatencyBoost = mode == ReflexMode::OnBoost;
    // No frame rate limit; markers report only, they do not steer the driver's optimizations.
    sleepMode.minimumIntervalUs = 0;
    sleepMode.bUseMarkersToOptimize = false;

    const NvAPI_Status result = NvAPI_D3D_SetSleepMode(m_device, &sleepMode);

    if(result != NVAPI_OK)
    {
        m_modeRefused = true;
        m_refusedMode = mode;
        m_status.detail = std::string("NvAPI_D3D_SetSleepMode(") + ReflexModeName(mode) + ") failed: " + NvapiMessage(result);
        Log::Warning("NVIDIA Reflex: " + m_status.detail);
        return false;
    }

    m_modeRefused = false;
    m_status.applied = mode;
    m_status.detail.clear();
    m_status.latencyValid = false;
    m_framesSinceLatencyReport = 0;

    NV_GET_SLEEP_STATUS_PARAMS sleepStatus{};
    sleepStatus.version = NV_GET_SLEEP_STATUS_PARAMS_VER;

    if(NvAPI_D3D_GetSleepStatus(m_device, &sleepStatus) == NVAPI_OK)
    {
        m_status.driverLowLatencyMode = sleepStatus.bLowLatencyMode != 0;
    }

    Log::Info(std::string("NVIDIA Reflex Low Latency: ") + ReflexModeName(mode) + ".");
    return true;
#else
    (void)mode;
    return false;
#endif
}

void ReflexLatency::SetMarker(int markerType)
{
#if defined(ENGINE_HAS_NVAPI)
    NV_LATENCY_MARKER_PARAMS marker{};
    marker.version = NV_LATENCY_MARKER_PARAMS_VER;
    marker.frameID = m_frameID;
    marker.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(markerType);
    const NvAPI_Status result = NvAPI_D3D_SetLatencyMarker(m_device, &marker);

    if(result != NVAPI_OK && m_status.detail.empty())
    {
        m_status.detail = "NvAPI_D3D_SetLatencyMarker(" + std::to_string(markerType) + ") failed: " + NvapiMessage(result);
        Log::Warning("NVIDIA Reflex: " + m_status.detail);
    }
#else
    (void)markerType;
#endif
}

void ReflexLatency::RefreshLatencyReport()
{
#if defined(ENGINE_HAS_NVAPI)
    // About 15 KiB: kept off the stack.
    auto report = std::make_unique<NV_LATENCY_RESULT_PARAMS>();
    report->version = NV_LATENCY_RESULT_PARAMS_VER;

    if(NvAPI_D3D_GetLatency(m_device, report.get()) != NVAPI_OK)
    {
        m_status.latencyValid = false;
        return;
    }

    // Report times are microseconds. A frame counts once every stage up to its GPU end was recorded.
    std::uint64_t latencySumMicroseconds = 0;
    std::uint64_t gpuFrameSumMicroseconds = 0;
    std::uint32_t completeFrames = 0;

    for(const auto& frame : report->frameReport)
    {
        const bool complete = frame.simStartTime != 0 && frame.gpuRenderEndTime > frame.simStartTime;

        if(!complete)
        {
            continue;
        }

        latencySumMicroseconds += frame.gpuRenderEndTime - frame.simStartTime;
        gpuFrameSumMicroseconds += frame.gpuFrameTimeUs;
        ++completeFrames;
    }

    m_status.latencyFrames = completeFrames;
    m_status.latencyValid = completeFrames > 0;

    if(m_status.latencyValid)
    {
        m_status.averageRenderLatencyMilliseconds =
            static_cast<double>(latencySumMicroseconds) / static_cast<double>(completeFrames) / 1000.0;
        m_status.averageGpuFrameMilliseconds =
            static_cast<double>(gpuFrameSumMicroseconds) / static_cast<double>(completeFrames) / 1000.0;
    }

    if(LogLatencyReports())
    {
        Log::Info(std::string("NVIDIA Reflex latency: mode ") + ReflexModeName(m_status.applied) + ", " +
            std::to_string(completeFrames) + " complete frames, render latency " +
            std::to_string(m_status.averageRenderLatencyMilliseconds) + " ms, GPU frame " +
            std::to_string(m_status.averageGpuFrameMilliseconds) + " ms, last sleep " +
            std::to_string(m_status.lastSleepMilliseconds) + " ms.");
    }
#endif
}
