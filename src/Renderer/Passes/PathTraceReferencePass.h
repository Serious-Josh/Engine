#pragma once

#include "Renderer/Texture.h"

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/RendererLightingMode.h"

#include <cstdint>
#include <string>

class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class RayTracingSceneResources;

// RTXPT.1 progressive reference path tracer. Owns one DXR pipeline per primary constructor (the same
// library, a different ray-generation export) and three renderer-lifetime resources: the RGBA32F running
// mean, an RGBA16F copy of it for presentation, and an R32_UINT statistics row. Every primary shares the
// resources. The Renderer owns scheduling, accumulation counts, resets and submission.
class PathTraceReferencePass
{
    public:
    // Statistics texel layout written by PathTraceReference.hlsl (cumulative, never reset on GPU).
    static constexpr unsigned int StatisticsWidth = 8;
    static constexpr unsigned int RaysLowTexel = 0;
    static constexpr unsigned int SamplesLowTexel = 2;
    static constexpr unsigned int RejectedSamplesTexel = 4;

    bool Initialize(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height,
        std::string& failureReason);
    // Recreates the size-dependent textures. Caller guarantees the queue is idle.
    bool Resize(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);
    void Shutdown();

    // Creates the pipeline of a non-reference primary on first use (Initialize creates the reference
    // pipeline). A failure, for example a cooked library from before RTXPT.2 R3 without the export, is
    // sticky for this pass lifetime and leaves the reference mode working.
    bool EnsurePrimaryPipeline(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        PathTracePrimary primary,
        std::string& failureReason);

    bool IsInitialized() const
    {
        return m_initialized;
    }

    // Records nothing unless the primary's pipeline exists (Initialize, or EnsurePrimaryPipeline).
    void Execute(
        PathTracePrimary primary,
        ID3D12GraphicsCommandList4* commandList,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height);

    // Copies the presentation image into the HDR scene colour consumed by tone mapping. Both are
    // RGBA16F at the scene extent; the caller checks that before recording.
    void CopyToSceneColor(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor);

    Texture& GetAccumulationTexture()
    {
        return m_accumulationTexture;
    }

    Texture& GetOutputTexture()
    {
        return m_outputTexture;
    }

    Texture& GetStatisticsTexture()
    {
        return m_statisticsTexture;
    }

    // Bytes of GPU memory owned by the size-dependent and statistics textures.
    std::uint64_t GetResourceBytes() const;

    private:
    bool CreateTextures(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        unsigned int width,
        unsigned int height);

    Texture m_accumulationTexture;
    Texture m_outputTexture;
    Texture m_statisticsTexture;

    static constexpr size_t PrimaryCount = 3;

    RayTracingPipeline m_pipelines[PrimaryCount];
    bool m_pipelineReady[PrimaryCount]{};
    bool m_pipelineFailed[PrimaryCount]{};

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
};
