#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Rtxdi/RtxdiLightBridge.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <cstddef>
#include <cstdint>

// RTXPT.3 R3.4 fixed initial-sampling configuration (Gate 2): candidates per path sample at the primary
// vertex. Uniform local-light selection.
// PT.2C: 16 stratified uniform local candidates (was 8): 0.45-0.55x the raw variance in local-light scenes for +0.14-0.5 ms of
// direct pass at 720p. Candidates trace no rays; only the selected sample is shadow-tested.
inline constexpr std::uint32_t RtxdiInitialLocalLightSamples = 16;
inline constexpr std::uint32_t RtxdiInitialInfiniteLightSamples = 4;
// PT.2B, the Reservoir environment strategy only (RtxdiEnvironmentConfig.hlsli): HDRI candidates drawn through Engine's CDF
// warp, and BSDF candidates, each tracing one ray that enters the HDRI when it escapes.
inline constexpr std::uint32_t RtxdiInitialEnvironmentSamples = 1;
inline constexpr std::uint32_t RtxdiInitialBrdfSamples = 1;


// PT.2B: the frame's RTXDI light data, a raw buffer the direct pass reads (RtxdiResources::GetLightDataSRVIndex). In order:
// currentLightCount records, previousLightCount records (RtxdiLightRecord, 64 bytes each), then currentLightCount
// current-to-previous and previousLightCount previous-to-current translation entries (int32, -1 for no counterpart).
struct RtxdiLightDataLayout
{
    static constexpr std::uint32_t RecordBytes = sizeof(RtxdiLightRecord);
    static constexpr std::uint32_t TranslationBytes = sizeof(std::int32_t);
    // Many-light M2: the initial allocation (both frames with the sun, the HDRI and one table light); the buffer grows with
    // the light count (RtxdiResources::RecordLightData).
    static constexpr std::uint32_t InitialBytes = 2 * 3 * (RecordBytes + TranslationBytes);
    // Many-light M3: shaders address the buffer with 32-bit byte offsets, so the whole layout must stay within 4 GiB.
    // Sizes and offsets are computed in 64 bits so a caller can check them against this before narrowing.
    static constexpr std::uint64_t MaximumBytes = UINT32_MAX;

    static constexpr std::uint64_t PreviousRecordsOffset(std::uint64_t currentCount) noexcept
    {
        return currentCount * RecordBytes;
    }

    static constexpr std::uint64_t TranslationOffset(std::uint64_t currentCount, std::uint64_t previousCount) noexcept
    {
        return (currentCount + previousCount) * RecordBytes;
    }

    static constexpr std::uint64_t Bytes(std::uint64_t currentCount, std::uint64_t previousCount) noexcept
    {
        return (currentCount + previousCount) * (RecordBytes + TranslationBytes);
    }
};

static_assert(RtxdiLightDataLayout::RecordBytes == 64);

// Root constant buffer b2 of the ReSTIR DI direct pass (PathTraceReSTIRDIDirectCS) only; identical layout to
// RtxdiConstants in PathTraceReference.hlsl. NrdHistoryConfidence.hlsli reads the leading rows up to motionIndex.
struct PathTraceRtxdiConstants
{
    RTXDI_LightBufferParameters lightBuffer{};
    RTXDI_ReservoirBufferParameters reservoirBuffer{};
    RTXDI_DIBufferIndices bufferIndices{};

    // RWStructuredBuffer<RTXDI_PackedDIReservoir> UAV in the resource descriptor heap.
    std::uint32_t reservoirBufferIndex = 0;
    std::uint32_t localLightSamples = 0;
    std::uint32_t infiniteLightSamples = 0;
    // WNP.2: 1 shadow-tests the selected initial sample before reuse (PathTraceReference.hlsl rtxdiInitialVisibility).
    std::uint32_t initialVisibility = 0;

    // PT.2B: the raw SRV of the frame's light data (RtxdiLightDataLayout) and its two record counts.
    std::uint32_t lightDataIndex = 0xFFFFFFFFu;
    std::uint32_t currentLightCount = 0;
    std::uint32_t previousLightCount = 0;
    // PT.2B, Reservoir environment strategy only: candidates per path sample (RTXDI's numEnvironmentSamples and
    // numBrdfSamples); 0 elsewhere.
    std::uint32_t environmentSamples = 0;

    std::uint32_t brdfSamples = 0;
    // 1 when previous reservoirs, previous surfaces, previous light records and the translation may be read.
    std::uint32_t historyValid = 0;
    // Buffer<float2> neighbor offsets and the G-buffer motion texture (current UV - previous UV, view-depth delta).
    std::uint32_t neighborOffsetsIndex = 0;
    std::uint32_t motionIndex = 0;

    // RTXPT.3 R3.5/R3.6: fused spatiotemporal reuse.
    RTXDI_RuntimeParameters runtime{};
    RTXDI_DISpatioTemporalResamplingParameters spatioTemporal{};

    std::uint32_t previousDepthIndex = 0;
    std::uint32_t previousSurfaceIndex = 0;
    std::uint32_t previousGeometricNormalIndex = 0;
    std::uint32_t previousMaterialIndex = 0;

    // PT.2A: 1 lets final shading reuse the initial-visibility result for a provably identical ray; 2 (diagnostics) also
    // traces that ray and counts disagreements.
    std::uint32_t reuseInitialVisibility = 0;
    // PT.2A diagnostics: RtxdiDiagnosticCounter UAV, or UINT32_MAX (no counting).
    std::uint32_t diagnosticCountersIndex = 0xFFFFFFFFu;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;

    // Linear depth is dot(forward, position - camera position) for the current and the previous camera.
    float previousCameraPosition[3]{};
    float padding2 = 0.0f;
    float cameraForward[3]{};
    float padding3 = 0.0f;
    float previousCameraForward[3]{};
    float padding4 = 0.0f;

    Matrix4 previousInverseCameraRelativeViewProjection;
};

static_assert(sizeof(RTXDI_LightBufferParameters) == 48);
static_assert(sizeof(RTXDI_ReservoirBufferParameters) == 16);
static_assert(sizeof(RTXDI_DIBufferIndices) == 32);
static_assert(sizeof(RTXDI_RuntimeParameters) == 16);
static_assert(sizeof(RTXDI_DISpatioTemporalResamplingParameters) == 48);
static_assert(sizeof(Matrix4) == 64);
static_assert(offsetof(PathTraceRtxdiConstants, reservoirBuffer) == 48);
static_assert(offsetof(PathTraceRtxdiConstants, bufferIndices) == 64);
static_assert(offsetof(PathTraceRtxdiConstants, reservoirBufferIndex) == 96);
static_assert(offsetof(PathTraceRtxdiConstants, lightDataIndex) == 112);
static_assert(offsetof(PathTraceRtxdiConstants, brdfSamples) == 128);
// HLSL starts each structure member on a 16-byte register.
static_assert(offsetof(PathTraceRtxdiConstants, runtime) == 144);
static_assert(offsetof(PathTraceRtxdiConstants, spatioTemporal) == 160);
static_assert(offsetof(PathTraceRtxdiConstants, previousDepthIndex) == 208);
static_assert(offsetof(PathTraceRtxdiConstants, reuseInitialVisibility) == 224);
static_assert(offsetof(PathTraceRtxdiConstants, previousCameraPosition) == 240);
static_assert(offsetof(PathTraceRtxdiConstants, previousInverseCameraRelativeViewProjection) == 288);
// Matches the HLSL cbuffer size DXC reflects for RtxdiConstants.
static_assert(sizeof(PathTraceRtxdiConstants) == 352);
