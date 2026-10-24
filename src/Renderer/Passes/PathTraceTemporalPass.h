#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Texture.h"

#include <cstdint>
#include <optional>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// Must match PathTraceTemporal.hlsl.
struct PathTraceTemporalConstants
{
    Matrix4 inverseCameraRelativeViewProjection;
    Matrix4 previousInverseCameraRelativeViewProjection;

    float cameraForward[3]{};
    float diffuseMaximumHistory = 32.0f;

    float previousCameraForward[3]{};
    float specularMaximumHistory = 32.0f;

    float previousCameraOffset[3]{};
    float depthTolerance = 0.02f;

    std::uint32_t resolution[2]{};
    std::uint32_t historyValid = 0;
    std::uint32_t present = 0;

    std::uint32_t currentDiffuseIndex = 0;
    std::uint32_t currentSpecularIndex = 0;
    std::uint32_t depthIndex = 0;
    std::uint32_t surfaceIndex = 0;

    std::uint32_t geometricNormalIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t motionIndex = 0;
    std::uint32_t previousDepthIndex = 0;

    std::uint32_t previousSurfaceIndex = 0;
    std::uint32_t previousGeometricNormalIndex = 0;
    std::uint32_t historyDiffuseIndex = 0;
    std::uint32_t historySpecularIndex = 0;

    std::uint32_t outputDiffuseIndex = 0;
    std::uint32_t outputSpecularIndex = 0;
    std::uint32_t presentationIndex = 0;
    float minimumHistoryWeight = 0.05f;

    float diffuseNormalThreshold = 0.90f;
    float specularNormalThreshold = 0.95f;
    float specularRoughnessStart = 0.1f;
    float specularRoughnessRange = 0.5f;

    // RTXPT D.2C.3 luminance second moments (RG32F: diffuse lighting, specular radiance).
    std::uint32_t historyMomentsIndex = 0;
    std::uint32_t outputMomentsIndex = 0;
    // SHARC.6A history prior (PathTraceTemporal.hlsl step 6) and its counters; 0 = off.
    std::uint32_t priorIndex = 0;
    std::uint32_t priorCountersIndex = 0;

    // DENOISE.3B extended diffuse history (PathTraceTemporal.hlsl step 7): its UAV (0 = off), a restart, its length cap
    // and whether the presentation uses it. The UAV index is filled in by Execute.
    std::uint32_t extendedIndex = 0;
    std::uint32_t extendedRestart = 0;
    float extendedMaximum = 256.0f;
    std::uint32_t extendedPresent = 0;
};

static_assert(sizeof(PathTraceTemporalConstants) == 2 * 64 + 11 * 16);

// RTXPT D.2B temporal reprojection and accumulation of the path tracer's extracted diffuse and specular radiance
// (diagnostics only; PathTraceTemporal.hlsl has the equations). Owns two RGBA16F ping-pong pairs, radiance in RGB and
// history length in alpha, and (RTXPT D.2C.3) one RG32F ping-pong pair of luminance second moments, at the scene
// extent, created on first use. All three follow the same slot, commit and invalidation.
//
// History is transactional. A frame reads the committed slot and writes the other one; CommitHistory, called only
// once the frame's command list has been submitted, makes the written slot the committed one. A frame that is
// recorded but never submitted leaves the committed slot untouched, and its written slot is simply written again by
// the next frame (a slot is only ever written by the frame that will commit it). Commands on the queue execute in
// submission order, so a submitted frame's reads of the committed slot precede any later frame's writes to it.
//
// History validity is the pass's own: the Renderer invalidates it when history cannot carry over (scene history
// reset, resize, failed frames, path-tracing frames without temporal processing, changed settings or lighting). It
// is independent of progressive accumulation and RTXDI reservoir history, so camera and object motion keep it.
class PathTraceTemporalPass
{
    public:
    static constexpr std::uint64_t BytesPerPixel = 4 * 8 + 2 * 8;

    // The resolution-independent part: the compute pipeline. Created on first use; a failure is sticky.
    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);

    // Creates the histories at the given extent when they are missing or of another extent, invalidating history.
    // Creation only: no GPU work is recorded.
    bool EnsureHistory(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height);

    // Releases the histories (resize). Caller guarantees the queue is idle.
    void ReleaseHistory();
    void Shutdown();

    // Forgets the committed history and any pending write: the next frame starts with history length one.
    void InvalidateHistory();

    // DENOISE.3B: creates the extended diffuse history (one RGBA16F texture at the histories' extent) when missing.
    // Creation only; it is released with the histories. Its contents are meaningful only after a restart.
    bool EnsureExtended(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    // Records one frame. constants carries the inputs, cameras and flags; the history indices and validity are
    // filled in here. Afterwards the outputs are in shader-resource state. The written slot is pending until
    // CommitHistory. priorCacheBinding (SHARC.6A): the frame's SHARC query binding when constants.priorIndex is set.
    // extended (DENOISE.3B): also update the extended diffuse history (EnsureExtended first), which is then in
    // shader-resource state too.
    void Execute(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        PathTraceTemporalConstants constants,
        unsigned int width,
        unsigned int height,
        D3D12_GPU_VIRTUAL_ADDRESS priorCacheBinding = 0,
        bool extended = false);

    // The frame recorded by Execute was submitted: its output becomes the history.
    void CommitHistory();

    // A recording that will not be submitted: its write is forgotten (the committed history is unaffected).
    void AbandonPendingHistory();

    bool IsInitialized() const
    {
        return m_initialized;
    }

    bool HasHistory() const
    {
        return m_historyValid;
    }

    // The slot written by the last recorded frame (pending or committed): its temporal diffuse lighting
    // (demodulated: multiply by the G-buffer diffuse albedo, PathTraceTemporal.hlsl) and specular radiance.
    Texture& GetDiffuseOutput()
    {
        return m_diffuse[m_lastWrittenSlot];
    }

    Texture& GetSpecularOutput()
    {
        return m_specular[m_lastWrittenSlot];
    }

    // The same slot's luminance second moments: R diffuse (demodulated lighting), G specular. Variance of the
    // incoming samples is max(M2 - Y(history RGB)^2, 0) with the matching output's RGB (PathTraceTemporal.hlsl).
    Texture& GetMomentsOutput()
    {
        return m_moments[m_lastWrittenSlot];
    }

    // DENOISE.3B extended diffuse history: demodulated diffuse lighting mean in RGB, its length in alpha.
    Texture& GetExtendedDiffuse()
    {
        return m_extended;
    }

    std::uint64_t GetResourceBytes() const;

    private:
    ComputePipeline m_pipeline;
    bool m_initialized = false;
    bool m_initializationFailed = false;

    Texture m_diffuse[2];
    Texture m_specular[2];
    Texture m_moments[2];
    Texture m_extended;
    unsigned int m_width = 0;
    unsigned int m_height = 0;

    // The committed slot (read by the next frame) and whether it holds history.
    unsigned int m_committedSlot = 0;
    bool m_historyValid = false;
    std::optional<unsigned int> m_pendingSlot;
    unsigned int m_lastWrittenSlot = 0;
};
