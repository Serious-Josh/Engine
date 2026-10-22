#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/GpuTimestampProfiler.h"
#include "Renderer/Texture.h"

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/RendererLightingMode.h"
#include "Renderer/Shader/ShaderCompiler.h"
#include "Renderer/Shader/ShaderLibraryCompileWorker.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class RayTracingSceneResources;

// SER S6B: the DXR library set every path-tracing primary uses, chosen once at startup.
//   SM66    PathTraceReference.hlsl and PathTraceReSTIRDI.hlsl (lib_6_6): the conventional path.
//   SM69    PathTraceReferenceSM69.hlsl and PathTraceReSTIRDISM69.hlsl: diagnostic only, the bit-exact non-SER
//           baseline of SM69SER.
//   SM69SER PathTraceReferenceSERSM69.hlsl and PathTraceReSTIRDISERSM69.hlsl: shader execution reordering on the
//           continuation rays of Reference, Raster Primary and ReSTIR DI; Pixel Centre traces conventionally.
// RTXPT D.1B.2 signal extraction (diagnostics only) adds, per variant, PathTraceReSTIRDIExtract[SM69|SERSM69].hlsl:
// the extraction direct pass, which never shares a library with the production direct pass.
enum class PathTraceLibraryVariant
{
    SM66,
    SM69,
    SM69SER
};

const char* PathTraceLibraryVariantName(PathTraceLibraryVariant variant);

// What the library selection needs to know about the device and process.
struct PathTraceLibraryCapabilities
{
    bool rayTracing = false;
    bool shaderModel69 = false;
    bool rayTracingTier12 = false;
    bool serActuallyReorders = false;
    // Packaged runtimes cook only the SM 6.6 libraries, so they always use SM66.
    bool packagedRuntime = false;
};

struct PathTraceLibrarySelection
{
    PathTraceLibraryVariant variant = PathTraceLibraryVariant::SM66;
    // Why this variant was chosen, for the startup log.
    std::string reason;
    // True when the override could not be honoured as written; the reason says why.
    bool warning = false;
};

// Pure selection rule. libraryOverride is ENGINE_PATH_TRACE_LIBRARIES (auto, sm66, sm69 or ser; empty when unset;
// case-insensitive). auto selects SM69SER when every SER capability is present, otherwise SM66. sm69 and ser are
// honoured only on capable devices and otherwise fall back to SM66 with a warning; an invalid value warns and
// means auto. No override bypasses a capability check, and packaged runtimes always get SM66.
PathTraceLibrarySelection SelectPathTraceLibraryVariant(
    std::string_view libraryOverride,
    const PathTraceLibraryCapabilities& capabilities);

// RTXPT D.1A: the path tracer's randomSeed for an accumulation run whose first frame is sequenceFrame frames after
// the last history reset. The MurmurHash3 32-bit finalizer of the folded frame number: a bijection on 32 bits that
// maps 0 to 0, so the first run after a reset keeps the historical seed-0 sequence.
std::uint32_t PathTraceSequenceSeed(std::uint64_t sequenceFrame);

// RTXPT.1 progressive reference path tracer. Owns one DXR pipeline per primary constructor (the same
// library, compiled once, a different ray-generation export) and three renderer-lifetime resources: the RGBA32F running
// mean, an RGBA16F copy of it for presentation, and an R32_UINT statistics row. Every primary shares the
// resources. RTXPT.3 ReSTIR DI adds the split direct and indirect pipelines and their direct radiance buffer,
// created on first use. The Renderer owns scheduling, accumulation counts, resets and submission.
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

    // Selects the library set for every primary (SelectPathTraceLibraryVariant decides it). The caller checks
    // device support. False, with the variant unchanged, once any library or pipeline exists.
    bool SelectLibraryVariant(PathTraceLibraryVariant variant);

    PathTraceLibraryVariant GetLibraryVariant() const
    {
        return m_libraryVariant;
    }

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

    // RTXPT.3: for PathTracePrimary::RasterReSTIRDI, the split pipeline pair (EnsurePrimaryPipeline creates it).
    bool IsPrimaryReady(PathTracePrimary primary) const
    {
        if(primary == PathTracePrimary::RasterReSTIRDI)
        {
            return IsReSTIRDISplitReady();
        }

        return m_initialized && m_pipelineReady[static_cast<size_t>(primary)];
    }

    // Records nothing unless the primary's pipeline exists (Initialize, or EnsurePrimaryPipeline), and never for
    // PathTracePrimary::RasterReSTIRDI, which records through ExecuteReSTIRDISplit. extract records the Raster
    // Primary extraction pipeline instead (EnsureSignalExtraction), and nothing for any other primary.
    void Execute(
        PathTracePrimary primary,
        ID3D12GraphicsCommandList4* commandList,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height,
        bool extract);

    // RTXPT.3 split ReSTIR DI: the direct-pass pipeline (from the ReSTIR DI library) and the indirect-pass
    // pipeline (from the reference library), together PathTracePrimary::RasterReSTIRDI. Created on first use; a
    // failure stays until a new revision of either library is accepted.
    bool EnsureReSTIRDISplitPipelines(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        std::string& failureReason);

    bool IsReSTIRDISplitReady() const
    {
        return m_initialized && m_splitPipelinesReady;
    }

    // The per-sample vertex-0 direct radiance the split passes exchange, holding samplesPerFrame samples per pixel.
    // Allocated on first use and replaced only when samplesPerFrame changes. A replaced buffer and its descriptor are
    // retired, not released: they stay alive until completedFence reaches lastSubmissionFence (the last submission
    // that may read them; the current recording does not). Returns the raw UAV index, or UINT_MAX when the buffer
    // cannot be allocated or addressed (the shader addresses it with 32-bit byte offsets). Creation only: no GPU
    // work is recorded, so allocating during recording is safe.
    unsigned int EnsureSplitDirectRadiance(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        std::uint32_t samplesPerFrame,
        std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence);

    // RTXPT D.1B.2: the split direct pass's per-sample vertex-0 diffuse radiance, written by the extraction direct
    // pass and read by the extraction indirect pass. Same layout, lifetime and return value as
    // EnsureSplitDirectRadiance.
    unsigned int EnsureSplitDirectDiffuse(
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        std::uint32_t samplesPerFrame,
        std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence);

    // Records the split ReSTIR DI frame: the direct pass, a UAV barrier on the direct radiance, then the indirect
    // pass. Records nothing unless both split pipelines and the direct radiance exist. The bindings are those of
    // Execute, plus the RTXDI constants (root b2) for the direct pass; constantsAddress must name the direct
    // radiance UAV. Each dispatch is timed in its own profiler region (PathTraceSplitDirect, PathTraceSplitIndirect).
    // extract records the extraction passes instead, which also need the direct diffuse buffer and the outputs
    // (constantsAddress names their UAVs too); without them nothing is recorded.
    void ExecuteReSTIRDISplit(
        ID3D12GraphicsCommandList4* commandList,
        GpuTimestampProfiler& profiler,
        unsigned int frameIndex,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height,
        D3D12_GPU_VIRTUAL_ADDRESS rtxdiConstantsAddress,
        bool extract);

    // RTXPT D.1B.2 diffuse/specular signal extraction (diagnostics only, never enabled by default). Creates, on first
    // use, the extraction pipelines of Raster Primary or split ReSTIR DI (the extraction exports of the libraries
    // the production pipelines use) and the two RGBA16F outputs at the current extent. A failure is sticky until a
    // new revision of the libraries is accepted. Creation only: no GPU work is recorded.
    bool EnsureSignalExtraction(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap,
        PathTracePrimary primary,
        std::string& failureReason);

    // The extraction outputs: diffuse RGB; specular RGB and the primary specular hit distance in alpha.
    Texture& GetDiffuseTexture()
    {
        return m_diffuseTexture;
    }

    Texture& GetSpecularTexture()
    {
        return m_specularTexture;
    }

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
    // True once any library or pipeline exists, after which the library selection cannot change.
    bool LibrarySelectionFixed() const;

    // Appended to library and pipeline failures outside SM66: names the variant and the sm66 override.
    std::string TroubleshootingHint() const;

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

    // RTXPT.3 split ReSTIR DI.
    RayTracingPipeline m_splitDirectPipeline;
    RayTracingPipeline m_splitIndirectPipeline;
    bool m_splitPipelinesReady = false;
    bool m_splitPipelinesFailed = false;
    std::string m_splitPipelineFailure;

    // Replaced per-sample buffers, kept until the GPU completes submissionFence.
    struct RetiredBuffer
    {
        Buffer buffer;
        std::uint64_t submissionFence = 0;
    };

    // A float3 per pixel and sample, sized for samplesPerFrame and replaced only when that changes.
    struct SplitSampleBuffer
    {
        Buffer buffer;
        std::uint32_t samples = 0;
        std::uint32_t failedSamples = 0;
        std::vector<RetiredBuffer> retired;
    };

    SplitSampleBuffer m_splitDirectRadiance;
    // RTXPT D.1B.2: the diffuse part of m_splitDirectRadiance (signal extraction only).
    SplitSampleBuffer m_splitDirectDiffuse;

    unsigned int EnsureSplitSampleBuffer(
        SplitSampleBuffer& target,
        const wchar_t* name,
        ID3D12Device* device,
        DescriptorHeap& descriptorHeap,
        std::uint32_t samplesPerFrame,
        std::uint64_t lastSubmissionFence,
        std::uint64_t completedFence);

    // Releases a per-sample buffer and every retired one. Caller guarantees the queue is idle.
    static void ReleaseSplitSampleBuffer(SplitSampleBuffer& target);

    // Releases the direct radiance buffers and every retired one. Caller guarantees the queue is idle.
    void ReleaseSplitDirectRadiance();

    // RTXPT D.1B.2 signal extraction, created on first use (EnsureSignalExtraction).
    RayTracingPipeline m_extractRasterPipeline;
    RayTracingPipeline m_extractSplitDirectPipeline;
    RayTracingPipeline m_extractSplitIndirectPipeline;
    bool m_extractRasterReady = false;
    bool m_extractRasterFailed = false;
    bool m_extractSplitReady = false;
    bool m_extractSplitFailed = false;
    std::string m_extractRasterFailure;
    std::string m_extractSplitFailure;
    Texture m_diffuseTexture;
    Texture m_specularTexture;

    // Creates the extraction outputs at the current extent when they do not exist.
    bool CreateExtractionTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    // The UAV barriers after a frame's last dispatch: accumulation and statistics, plus the extraction outputs.
    void RecordOutputBarriers(ID3D12GraphicsCommandList* commandList, bool extract);

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
    PathTraceLibraryVariant m_libraryVariant = PathTraceLibraryVariant::SM66;
};
