#pragma once

#include "Renderer/Texture.h"

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/RendererLightingMode.h"
#include "Renderer/Shader/ShaderCompiler.h"
#include "Renderer/Shader/ShaderLibraryCompileWorker.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class RayTracingSceneResources;

// RTXPT.1 progressive reference path tracer. Owns one DXR pipeline per primary constructor (the same
// library, compiled once, a different ray-generation export) and three renderer-lifetime resources: the RGBA32F running
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

    // Async DXR preparation (Gate 2): polls the worker for the library a primary needs, without blocking.
    // A completed compilation is accepted only while every source it read is unchanged; a stale result is
    // resubmitted. Accepting a new revision clears the sticky pipeline failures of the primaries built from
    // it, so a fixed shader can be retried. A failure stays until its sources change. Initialize and
    // EnsurePrimaryPipeline then find the accepted library current and do not compile.
    ShaderLibraryPreparation PrepareLibrary(
        ShaderLibrary& shaderLibrary,
        ShaderLibraryCompileWorker& worker,
        PathTracePrimary primary);

    bool IsInitialized() const
    {
        return m_initialized;
    }

    bool IsPrimaryReady(PathTracePrimary primary) const
    {
        return m_initialized && m_pipelineReady[static_cast<size_t>(primary)];
    }

    // Records nothing unless the primary's pipeline exists (Initialize, or EnsurePrimaryPipeline).
    // rtxdiConstantsAddress (root b2) is bound only for PathTracePrimary::RasterReSTIRDI, whose pipeline alone
    // has that parameter; it must then be non-zero.
    void Execute(
        PathTracePrimary primary,
        ID3D12GraphicsCommandList4* commandList,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height,
        D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstantsAddress = 0);

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

    // The compiled library at path, compiled on first request and reused while its sources are unchanged;
    // null when it does not compile.
    const ShaderLibraryCompileResult* AcquireLibrary(ShaderLibrary& shaderLibrary, const std::filesystem::path& path);

    struct CompiledLibrary
    {
        std::filesystem::path path;
        ShaderLibraryCompileResult library;
    };

    // The current failed worker compilation of a library, kept so it is not resubmitted until its
    // sources change.
    struct FailedLibrary
    {
        std::filesystem::path path;
        ShaderLibraryCompileOutcome outcome;
    };

    // Shutdown without discarding compiled or failed libraries, which outlive a failed Initialize.
    void ReleaseResources();

    ShaderLibraryPreparation SubmitLibrary(
        ShaderLibrary& shaderLibrary,
        ShaderLibraryCompileWorker& worker,
        const std::filesystem::path& path);

    // Every primary shares PathTraceReference.hlsl except ReSTIR DI, so this holds at most two libraries
    // until Shutdown. Pipelines keep their own copies inside their state objects.
    std::vector<CompiledLibrary> m_libraries;
    std::vector<FailedLibrary> m_failedLibraries;

    Texture m_accumulationTexture;
    Texture m_outputTexture;
    Texture m_statisticsTexture;

    static constexpr size_t PrimaryCount = 4;

    RayTracingPipeline m_pipelines[PrimaryCount];
    bool m_pipelineReady[PrimaryCount]{};
    bool m_pipelineFailed[PrimaryCount]{};
    std::string m_pipelineFailure[PrimaryCount];

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
};
