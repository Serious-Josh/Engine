#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Rtxdi/RtxdiLightBridge.h"

#include "Rtxdi/DI/ReSTIRDI.h"

#include <cstddef>
#include <cstdint>

// RTXPT.3 R3.4 fixed initial-sampling configuration (Gate 2): candidates per path sample at the primary
// vertex. Uniform local-light selection; no BRDF or environment-region candidates.
inline constexpr std::uint32_t RtxdiInitialLocalLightSamples = 8;
inline constexpr std::uint32_t RtxdiInitialInfiniteLightSamples = 4;

// Root constant buffer b2 of PathTraceRasterPrimaryReSTIRDIRayGeneration only; identical layout to
// RtxdiConstants in PathTraceReference.hlsl. Light records are packed two per uint4; the light translation
// (current-to-previous entries, then previous-to-current entries) four per uint4.
struct PathTraceRtxdiConstants
{
    static constexpr std::uint32_t RecordWords = RtxdiLightSet::Capacity * 2;
    static constexpr std::uint32_t TranslationWords = RtxdiLightSet::Capacity * 2;

    RTXDI_LightBufferParameters lightBuffer{};
    RTXDI_ReservoirBufferParameters reservoirBuffer{};
    RTXDI_DIBufferIndices bufferIndices{};

    // RWStructuredBuffer<RTXDI_PackedDIReservoir> UAV in the resource descriptor heap.
    std::uint32_t reservoirBufferIndex = 0;
    std::uint32_t localLightSamples = 0;
    std::uint32_t infiniteLightSamples = 0;
    std::uint32_t padding = 0;

    // (kind, sourceIndex) for each RTXDI light index.
    std::uint32_t lightRecords[RecordWords]{};

    // RTXPT.3 R3.5/R3.6: fused spatiotemporal reuse.
    RTXDI_RuntimeParameters runtime{};
    RTXDI_DISpatioTemporalResamplingParameters spatioTemporal{};

    // 1 when previous reservoirs, previous surfaces and the translation below may be read.
    std::uint32_t historyValid = 0;
    // Buffer<float2> neighbor offsets and the G-buffer motion texture (current UV - previous UV, view-depth delta).
    std::uint32_t neighborOffsetsIndex = 0;
    std::uint32_t motionIndex = 0;
    std::uint32_t currentLightCount = 0;

    std::uint32_t previousLightCount = 0;
    std::uint32_t previousDepthIndex = 0;
    std::uint32_t previousSurfaceIndex = 0;
    std::uint32_t previousGeometricNormalIndex = 0;

    std::uint32_t previousMaterialIndex = 0;
    std::uint32_t padding2[3]{};

    // Linear depth is dot(forward, position - camera position) for the current and the previous camera.
    float previousCameraPosition[3]{};
    float padding3 = 0.0f;
    float cameraForward[3]{};
    float padding4 = 0.0f;
    float previousCameraForward[3]{};
    float padding5 = 0.0f;

    Matrix4 previousInverseCameraRelativeViewProjection;

    std::int32_t lightTranslation[TranslationWords]{};
};

static_assert(PathTraceRtxdiConstants::RecordWords % 4 == 0, "Light records must fill whole uint4 registers.");
static_assert(PathTraceRtxdiConstants::TranslationWords % 4 == 0, "Translation must fill whole int4 registers.");
static_assert(sizeof(RTXDI_LightBufferParameters) == 48);
static_assert(sizeof(RTXDI_RuntimeParameters) == 16);
static_assert(sizeof(RTXDI_DISpatioTemporalResamplingParameters) == 48);
static_assert(sizeof(Matrix4) == 64);
static_assert(offsetof(PathTraceRtxdiConstants, reservoirBuffer) == 48);
static_assert(offsetof(PathTraceRtxdiConstants, bufferIndices) == 64);
static_assert(offsetof(PathTraceRtxdiConstants, reservoirBufferIndex) == 96);
static_assert(offsetof(PathTraceRtxdiConstants, lightRecords) == 112);
static_assert(offsetof(PathTraceRtxdiConstants, runtime) == 112 + 4 * PathTraceRtxdiConstants::RecordWords);
static_assert(offsetof(PathTraceRtxdiConstants, historyValid) == offsetof(PathTraceRtxdiConstants, runtime) + 64);
static_assert(offsetof(PathTraceRtxdiConstants, previousCameraPosition) ==
    offsetof(PathTraceRtxdiConstants, historyValid) + 48);
static_assert(offsetof(PathTraceRtxdiConstants, previousInverseCameraRelativeViewProjection) ==
    offsetof(PathTraceRtxdiConstants, previousCameraPosition) + 48);
static_assert(offsetof(PathTraceRtxdiConstants, lightTranslation) ==
    offsetof(PathTraceRtxdiConstants, previousInverseCameraRelativeViewProjection) + 64);
// Matches the HLSL cbuffer size DXC reflects for RtxdiConstants.
static_assert(sizeof(PathTraceRtxdiConstants) == 1392);
