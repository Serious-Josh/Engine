#pragma once

#include <cstdint>
#include <string>

// NVIDIA Reflex Low Latency (NVAPI) as the renderer sees it: the mode a frame asks for and what the driver reports.
// Kept free of NVAPI types so that DebugUI and the Editor can show it without the SDK.

// NVIDIA's three user-facing modes (RTX UI Developer Guidelines: "NVIDIA Reflex Low Latency", Off / On / On + Boost).
enum class ReflexMode
{
    Off,
    On,
    // On, plus NV_SET_SLEEP_MODE_PARAMS::bLowLatencyBoost: the GPU keeps its maximum clocks, which saves more latency in
    // CPU-bound frames at slightly higher power.
    OnBoost
};

const char* ReflexModeName(ReflexMode mode) noexcept;

// Ordered by how far initialization got.
enum class ReflexAvailability
{
    // Built without the NVAPI SDK (ENGINE_ENABLE_NVAPI).
    NotCompiled,
    // No frame has asked for Reflex yet; NVAPI is initialized on the first request.
    NotInitialized,
    // NvAPI_Initialize failed: no NVIDIA GPU or driver (the detail holds NVAPI's message).
    NvapiUnavailable,
    // NVAPI runs, but NvAPI_D3D_GetSleepStatus refused this device (GPU or driver without Reflex).
    Unsupported,
    Available
};

const char* ReflexAvailabilityName(ReflexAvailability availability) noexcept;

struct ReflexStatus
{
    ReflexAvailability availability = ReflexAvailability::NotInitialized;
    // The mode the last frame asked for, and the mode NvAPI_D3D_SetSleepMode last accepted.
    ReflexMode requested = ReflexMode::Off;
    ReflexMode applied = ReflexMode::Off;
    // NV_GET_SLEEP_STATUS_PARAMS::bLowLatencyMode after the last mode change: the driver's own view.
    bool driverLowLatencyMode = false;
    // The last NVAPI failure, or why Reflex is unavailable (empty when nothing failed).
    std::string detail;

    // CPU time of the last NvAPI_D3D_Sleep (0 when Reflex is off).
    double lastSleepMilliseconds = 0.0;

    // NvAPI_D3D_GetLatency averages over the reported frames that completed every stage: simulation start to the end of
    // the frame's GPU work, and the GPU's frame-to-frame time. Valid once the driver has filled its report (about 90 frames).
    bool latencyValid = false;
    std::uint32_t latencyFrames = 0;
    double averageRenderLatencyMilliseconds = 0.0;
    double averageGpuFrameMilliseconds = 0.0;
};

// One line for logs and the Editor's runtime status.
std::string DescribeReflexStatus(const ReflexStatus& status);
