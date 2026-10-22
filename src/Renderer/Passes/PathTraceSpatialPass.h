#pragma once

#include "Math/Matrix4.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Texture.h"

#include <cstdint>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// RTXPT D.3B.0 denoiser diagnostic views (PathTraceSpatial.hlsl step 6). Diagnostics only; Normal is the renderer's
// ordinary presentation.
enum class PathTraceDenoiserView : std::uint32_t
{
    Normal = 0,
    TemporalRadiance = 1,
    SpatialRadiance = 2,
    Difference = 3,
    Uncertainty = 4,
    History = 5,
    Raw = 6
};

struct PathTraceDenoiserVisualization
{
    PathTraceDenoiserView view = PathTraceDenoiserView::Normal;
    // 0: diffuse lighting (demodulated); 1: specular radiance.
    std::uint32_t signal = 0;
    // Radiance views are scaled by 2^exposureStops before the normal tone map; the difference view too.
    float exposureStops = 0.0f;
    float differenceAmplification = 16.0f;
    // The uncertainty view maps log10(sigma / Y) from this floor (blue) to 0 (red).
    float uncertaintyFloorLog10 = -3.0f;
};

// Views shown as display-encoded colours without tone mapping.
inline bool IsScalarPathTraceDenoiserView(PathTraceDenoiserView view)
{
    return view == PathTraceDenoiserView::Difference || view == PathTraceDenoiserView::Uncertainty ||
        view == PathTraceDenoiserView::History;
}

// Must match PathTraceSpatial.hlsl.
struct PathTraceSpatialConstants
{
    Matrix4 inverseCameraRelativeViewProjection;

    float cameraForward[3]{};
    // Plane distance, relative to the centre's view depth, at which the depth weight falls to 1/e.
    float depthSigma = 0.01f;

    std::uint32_t resolution[2]{};
    std::uint32_t diffuseIndex = 0;
    std::uint32_t specularIndex = 0;

    std::uint32_t momentsIndex = 0;
    std::uint32_t depthIndex = 0;
    std::uint32_t surfaceIndex = 0;
    std::uint32_t geometricNormalIndex = 0;

    std::uint32_t materialIndex = 0;
    std::uint32_t outputIndex = 0;
    // Luminance stop width in standard deviations of the reconstructed value. RTXPT D.3B: 16 (D.3A proposed 8);
    // the stop loses energy on right-skewed noise, and 16 brought young-history bias from about -2.3% to -1.5%
    // for no measured texture cost and a small edge cost (build/rtxpt-d3b/REPORT.md).
    float luminanceSigma = 16.0f;
    float normalPower = 64.0f;

    float geometricNormalThreshold = 0.90f;
    float specularRoughnessMinimum = 0.25f;
    float specularRoughnessTolerance = 0.15f;
    float diffuseMaximumHistory = 32.0f;

    // RTXPT D.3B.0 diagnostic views (0: normal presentation).
    std::uint32_t visualizationMode = 0;
    std::uint32_t visualizationSignal = 0;
    float exposureScale = 1.0f;
    float differenceScale = 16.0f;

    std::uint32_t rawDiffuseIndex = 0;
    std::uint32_t rawSpecularIndex = 0;
    float uncertaintyFloorLog10 = -3.0f;
    std::uint32_t padding = 0;
};

static_assert(sizeof(PathTraceSpatialConstants) == 64 + 7 * 16);

// Must match PathTraceSpatialAtrous.hlsl (RTXPT D.3C.2).
struct PathTraceAtrousConstants
{
    Matrix4 inverseCameraRelativeViewProjection;

    float cameraForward[3]{};
    float depthSigma = 0.01f;

    std::uint32_t resolution[2]{};
    std::uint32_t temporalDiffuseIndex = 0;
    std::uint32_t temporalSpecularIndex = 0;

    std::uint32_t momentsIndex = 0;
    std::uint32_t depthIndex = 0;
    std::uint32_t surfaceIndex = 0;
    std::uint32_t geometricNormalIndex = 0;

    std::uint32_t materialIndex = 0;
    std::uint32_t intermediateDiffuseIndex = 0;
    std::uint32_t intermediateSpecularIndex = 0;
    std::uint32_t outputIndex = 0;

    float luminanceSigma = 16.0f;
    float normalPower = 64.0f;
    float geometricNormalThreshold = 0.90f;
    float specularRoughnessMinimum = 0.25f;

    float specularRoughnessTolerance = 0.15f;
    float diffuseMaximumHistory = 32.0f;
    std::uint32_t visualizationMode = 0;
    std::uint32_t visualizationSignal = 0;

    float exposureScale = 1.0f;
    float differenceScale = 16.0f;
    std::uint32_t padding[2]{};
};

static_assert(sizeof(PathTraceAtrousConstants) == 64 + 7 * 16);

// RTXPT D.3B: one 5x5 variance-guided, edge-aware spatial filtering iteration over the temporal pass's outputs
// (diagnostics only; PathTraceSpatial.hlsl has the equations). Reads this frame's temporal diffuse lighting
// (demodulated), specular radiance, luminance second moments and the G-buffer; writes remodulated diffuse plus
// specular radiance to the presentation texture on surface pixels. It writes no history: the temporal history never
// sees spatially filtered values.
//
// RTXPT D.3C.2 adds one or two 3x3 a-trous iterations (steps 1 and 2, PathTraceSpatialAtrous.hlsl) as an alternative
// to the 5x5 pass, which stays the zero-iteration path and the fallback. Two iterations use one intermediate: per
// signal an RGBA16F texture (RGB value, A the signed standard deviation, see the shader) at the scene extent,
// created on first use and released on resize. It is rewritten by iteration 1 every frame before iteration 2 reads
// it, so it carries nothing between frames.
class PathTraceSpatialPass
{
    public:
    // The compute pipelines (normal and D.3B.0 diagnostic entry points), created on first use; a failure is sticky.
    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);
    void Shutdown();

    bool IsInitialized() const
    {
        return m_initialized;
    }

    // Records the filter. The inputs must be shader-readable and the output an unordered-access view; the caller
    // owns their transitions.
    void Execute(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        PathTraceSpatialConstants constants,
        unsigned int width,
        unsigned int height);

    // The a-trous pipelines (normal and diagnostic entry points), created on first use; a failure is sticky.
    bool InitializeAtrous(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);

    // Creates the intermediate at the given extent when it is missing or of another extent. Creation only.
    bool EnsureIntermediate(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width,
        unsigned int height);

    // Releases the intermediate (resize). Caller guarantees the queue is idle.
    void ReleaseIntermediate();

    // Records one or two a-trous iterations (iterations 1 or 2) from the same inputs as Execute, which must be in the
    // same states; the last iteration writes the presentation (diagnostics: a D.3B.0 view 2 or 3). Two iterations
    // need EnsureIntermediate first. constants carries the 5x5 pass's values, recorded as its last constants too.
    void ExecuteAtrous(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        const PathTraceSpatialConstants& constants,
        std::uint32_t iterations,
        unsigned int width,
        unsigned int height);

    Texture& GetIntermediateDiffuse()
    {
        return m_intermediateDiffuse;
    }

    Texture& GetIntermediateSpecular()
    {
        return m_intermediateSpecular;
    }

    std::uint64_t GetIntermediateBytes() const;

    // The constants of the last recorded dispatch (diagnostics: the CPU reference replays them).
    const PathTraceSpatialConstants& GetLastConstants() const
    {
        return m_lastConstants;
    }

    private:
    ComputePipeline m_pipeline;
    ComputePipeline m_diagnosticPipeline;
    PathTraceSpatialConstants m_lastConstants{};
    bool m_initialized = false;
    bool m_initializationFailed = false;

    // RTXPT D.3C.2 a-trous: [0] first iteration to the intermediate, [1] first iteration to the presentation, [2]
    // second iteration, [3] and [4] the diagnostic variants of [1] and [2].
    ComputePipeline m_atrousPipelines[5];
    bool m_atrousInitialized = false;
    bool m_atrousInitializationFailed = false;
    Texture m_intermediateDiffuse;
    Texture m_intermediateSpecular;
    unsigned int m_intermediateWidth = 0;
    unsigned int m_intermediateHeight = 0;
};
