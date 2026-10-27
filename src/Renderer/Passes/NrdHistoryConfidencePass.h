#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Texture.h"

#include <cstdint>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// Must match NrdHistoryConfidence.hlsli NrdHistoryConfidenceProbeConstants. Execute fills in the resolution, probe
// resolution and resource indices; the caller fills in the camera, G-buffer, light and history fields.
struct NrdHistoryConfidenceProbeConstants
{
    Matrix4 inverseViewProjection;

    std::uint32_t resolution[2]{};
    std::uint32_t probeResolution[2]{};

    float cameraPosition[3]{};
    std::uint32_t depthIndex = 0;

    std::uint32_t surfaceIndex = 0;
    std::uint32_t geometricNormalIndex = 0;
    std::uint32_t recordBufferIndex = 0;
    std::uint32_t signalIndex = 0;

    std::uint32_t geometryIndex = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t previousValid = 0;
    // R.9B.2: the probe-state UAV (filled in by Execute).
    std::uint32_t stateIndex = 0;

    float sunDirectionToLight[3]{};
    float sunCosAngularRadius = 1.0f;

    float sunIrradiance[3]{};
    std::uint32_t environmentTextureIndex = 0;

    std::uint32_t environmentConditionalCDFIndex = 0;
    std::uint32_t environmentMarginalCDFIndex = 0;
    std::uint32_t environmentWidth = 0;
    std::uint32_t environmentHeight = 0;

    float environmentTotalWeight = 0.0f;
    std::uint32_t padding1[3]{};
};

static_assert(sizeof(NrdHistoryConfidenceProbeConstants) == 64 + 8 * 16);

// Must match NrdHistoryConfidence.hlsli NrdHistoryConfidenceFilterConstants.
struct NrdHistoryConfidenceFilterConstants
{
    std::uint32_t probeResolution[2]{};
    std::uint32_t inputIndex = 0;
    std::uint32_t outputIndex = 0;

    std::uint32_t geometryIndex = 0;
    std::uint32_t confidenceIndex = 0;
    std::uint32_t step = 1;
    std::uint32_t lastPass = 0;

    float recordCameraPosition[3]{};
    float sensitivity = 0.0f;

    float minConfidence = 0.0f;
    float planeSensitivity = 0.0f;
    std::uint32_t confidenceStrength = 1;
    std::uint32_t padding = 0;
};

static_assert(sizeof(NrdHistoryConfidenceFilterConstants) == 4 * 16);

// NRD RELAX R.9B diagnostics: paired-visibility probes and the diffuse history confidence they produce
// (NrdHistoryConfidence.hlsli has the algorithm). One probe per 2x2 pixel block. R.9C optionally binds the final
// confidence to regular RELAX; generation and resource ownership remain here.
//
// Owns, at the probe extent: the probe records (32 bytes each, one raw buffer, read and rewritten in place by the probe
// that owns them), the raw change signal (RG32F: T dV, T), the record geometry for the filter (RGBA32F: world position,
// packed normal; w = 0 without a surface), two RG32F filter targets and the final confidence (R16F). All of them are in
// the previous frame's pixel space. Created on first use, recreated on a resize (which loses the records).
class NrdHistoryConfidencePass
{
    public:
    // R.9B experimental mapping constants (NrdHistoryConfidence.hlsli CSFilter).
    static constexpr std::uint32_t FilterPasses = 3;
    static constexpr float Sensitivity = 2.0f;
    static constexpr float MinConfidence = 0.1f;
    // Plane distance, as a fraction of the distance from the recording camera, at which a neighbour's weight reaches 0.
    static constexpr float PlaneSensitivity = 0.02f;
    // Shadow rays per probe and frame: the previous record's retrace and the new record's visibility.
    static constexpr std::uint32_t RaysPerProbe = 2;

    // The resolution-independent part: the probe and filter pipelines. A failure is sticky until Shutdown.
    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);

    // Creates the resources for the render extent when missing or of another extent. Returns false on failure; sets
    // recreated when the records were (re)created, i.e. hold no previous frame.
    bool EnsureResources(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height,
        bool& recreated);

    // Caller guarantees the queue is idle.
    void ReleaseResources();
    void Shutdown();

    // Records the probe pass and the filter passes. constants carries the camera, G-buffer, light and history fields;
    // the G-buffer textures must be shader-readable and the TLAS built. Afterwards the signal and confidence textures
    // are shader-readable. False when nothing usable was recorded.
    bool Execute(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        NrdHistoryConfidenceProbeConstants constants,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstants,
        D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstants,
        D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure,
        const float recordCameraPosition[3],
        std::uint32_t confidenceStrength);

    bool HasResources() const
    {
        return m_records.GetResource() != nullptr;
    }

    // The raw change signal (view 4) and the final confidence (view 5), at the probe extent, previous-frame space.
    // R.9F strength > 1 changes that same final texture, including View 5; no separate display resource.
    Texture& GetSignal()
    {
        return m_signal;
    }

    Texture& GetConfidence()
    {
        return m_confidence;
    }

    // R.9B.2: the evaluated record's state (R8_UINT, NrdHistoryConfidence.hlsli NRD_CONFIDENCE_STATE_*) for view 6.
    Texture& GetState()
    {
        return m_state;
    }

    unsigned int GetProbeWidth() const
    {
        return m_probeWidth;
    }

    unsigned int GetProbeHeight() const
    {
        return m_probeHeight;
    }

    std::uint64_t GetResourceBytes() const;

    private:
    ComputePipeline m_probePipeline;
    ComputePipeline m_filterPipeline;
    bool m_initialized = false;
    bool m_initializationFailed = false;

    Buffer m_records;
    Texture m_signal;
    Texture m_geometry;
    Texture m_filter[2];
    Texture m_confidence;
    Texture m_state;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    unsigned int m_probeWidth = 0;
    unsigned int m_probeHeight = 0;
};
