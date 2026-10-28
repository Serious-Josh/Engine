#pragma once

#include "../../../assets/Shaders/PathTraceSharcDiagnosticConfig.hlsli"

#include "Renderer/Buffer.h"
#include "Renderer/GpuTimestampProfiler.h"
#include "Renderer/Texture.h"

#include "Renderer/RayTracing/RayTracingPipeline.h"
#include "Renderer/RayTracing/RayTracingPipelineWorker.h"
#include "Renderer/RendererLightingMode.h"
#include "Renderer/Shader/ShaderCompiler.h"
#include "Renderer/Shader/ShaderLibraryCompileWorker.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>

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

// SHARC.4D/4D-Q learning libraries of each variant (experimental, default off). Inline inserts at the end of each path
// (PathTraceReferenceRadianceCache*.hlsl); Deferred stores a training record per path for a later compute insertion
// (PathTraceReferenceRadianceCacheDeferred*.hlsl); DeferredLate is Deferred with the whole record stored at the end of
// the path (diagnostics, the SHARC.4D-Q shader-floor comparison). SHARC.4D-R (diagnostics): Query renders with query-only
// hooks (PathTraceReferenceRadianceCacheQuery*.hlsl); Training is the Deferred library's training-only ray generation
// (PathTraceReferenceRadianceCacheTraining*.hlsl), dispatched by ExecuteRadianceCacheTraining. SHARC.4D-S
// (diagnostics): QueryTermination is Query with vertex-1 path termination (a timing probe; its images are wrong).
// SHARC.4E (experimental): Render completes eligible vertex-1 paths with cached radiance
// (PathTraceReferenceRadianceCacheRender*.hlsl); SHARC.4E.1: RenderExtraction is the same library's signal-extraction
// ray generation, for frames the path-tracing denoiser filters. SHARC.5B: RenderSplitIndirect[Extraction] are the same
// library's ReSTIR DI indirect-pass exports (normal, and with signal extraction), for ExecuteReSTIRDISplit. NRD.RELAX.2B:
// the *RawDiffuseHitDistance entries are those two extraction exports from the render library's raw-distance build
// (PathTraceReferenceRadianceCacheRenderRawDiffuseHitDistance*.hlsl), for frames that also write the raw diffuse hit distance.
enum class PathTraceRadianceCacheLibrary
{
    Inline,
    Deferred,
    DeferredLate,
    Query,
    Training,
    QueryTermination,
    Render,
    RenderExtraction,
    RenderSplitIndirect,
    RenderSplitIndirectExtraction,
    RenderExtractionRawDiffuseHitDistance,
    RenderSplitIndirectExtractionRawDiffuseHitDistance,
    Count
};

// ER.1 Phase 2: what a path-traced configuration records, for the DXR libraries it must have before activation.
struct PathTraceLibraryNeeds
{
    PathTracePrimary primary = PathTracePrimary::Camera;
    // ER.1 Phase 4: signal extraction (any path-tracing denoiser) records the extraction pipelines.
    bool extraction = false;
    // The raw diffuse hit distance extraction (NRD RELAX and the hybrid).
    bool rawDiffuseHitDistance = false;
    // SHARC: the sparse training library and the render library that substitutes cached radiance.
    bool radianceCache = false;
    PathTraceRadianceCacheLibrary radianceCacheRender = PathTraceRadianceCacheLibrary::Render;
};

// One DXR library a configuration acquires. An optional library is a GI.6 or DI.1 RayQuery twin: its failure keeps
// the original TraceRay pipeline, so it never blocks activation.
struct PathTraceLibraryRequirement
{
    std::filesystem::path path;
    bool optional = false;
};

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

    // ER.1 Phase 2: the same preparation for any DXR library this pass acquires.
    ShaderLibraryPreparation PrepareLibrary(
        ShaderLibrary& shaderLibrary,
        ShaderLibraryCompileWorker& worker,
        const std::filesystem::path& path);

    // ER.1 Phase 2: every library the pipelines of a configuration acquire, RayQuery twins included, so the worker
    // can compile all of them before activation and no pipeline creation compiles on the render thread.
    std::vector<PathTraceLibraryRequirement> GetRequiredLibraries(const PathTraceLibraryNeeds& needs) const;

    // ER.1 Phase 4: every DXR pipeline the configuration's creation paths build (the same description builders), with
    // the RayQuery twin InitializeVisibilityPipelines pairs with each. Optional entries are twins.
    struct PipelineRequirement
    {
        RayTracingPipelineDescription description;
        bool optional = false;
    };

    std::vector<PipelineRequirement> GetRequiredPipelines(const PathTraceLibraryNeeds& needs) const;

    // ER.1 Phase 4: polls the pipeline worker for every required pipeline, never blocking. Requires the configuration's
    // libraries accepted (PrepareLibrary Ready). Ready once each pipeline is live in its slot or prepared for adoption at
    // the current library revision; PreparingPipelines while any is queued or creating; Failed when a required one
    // failed (a failed twin counts as Ready: its pipeline keeps TraceRay). The creation paths then adopt the prepared
    // pipelines (RayTracingPipeline::Swap) instead of calling CreateStateObject on the render thread.
    ShaderLibraryPreparation PreparePipelines(RayTracingPipelineWorker& worker, ID3D12Device5* device,
        const PathTraceLibraryNeeds& needs);

    struct PipelineStatistics
    {
        std::uint64_t adopted = 0;
        std::uint64_t synchronous = 0;
        std::uint64_t submitted = 0;
        std::uint64_t failures = 0;
        std::uint64_t discarded = 0;
        std::size_t prepared = 0;
    };

    PipelineStatistics GetPipelineStatistics() const
    {
        PipelineStatistics statistics = m_pipelineStatistics;
        statistics.prepared = m_preparedPipelines.size();
        return statistics;
    }

    // Diagnostics only: background creations whose description key contains this text fail (empty: none).
    void SetPipelineFailureForTesting(std::string keyFragment)
    {
        m_pipelineFailureForTesting = std::move(keyFragment);
    }

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
    // NRD.RELAX.2B: rawDiffuseHitDistance (with extract) selects the raw-distance extraction pipeline, or the matching raw
    // SHARC library, when it and the raw diffuse hit distance texture exist; otherwise the legacy extraction records,
    // writing no raw distance. WroteRawDiffuseHitDistance reports which.
    // Returns whether a DispatchRays was recorded (diagnostic metadata only).
    bool Execute(
        PathTracePrimary primary,
        ID3D12GraphicsCommandList4* commandList,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height,
        bool extract,
        D3D12_GPU_VIRTUAL_ADDRESS radianceCacheConstants = 0,
        PathTraceRadianceCacheLibrary radianceCacheLibrary = PathTraceRadianceCacheLibrary::Inline,
        bool rawDiffuseHitDistance = false);

    // SHARC.4D learning (experimental, default off): creates, on first use, the Raster Primary pipeline of a cache
    // library (PathTraceRadianceCacheLibrary, with the cache binding at b3). It traces and renders exactly as the
    // production pipeline and also inserts or records D6 training samples. A failure is sticky until a new revision of
    // the libraries is accepted. Creation only.
    bool EnsureRadianceCacheLearning(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        std::string& failureReason,
        PathTraceRadianceCacheLibrary library = PathTraceRadianceCacheLibrary::Inline);

    // SHARC.4D-R sparse training (diagnostics): after Execute in the same list, the training library's ray generation
    // over `threads` training threads (one per pixel tile), with the frame's path-trace constants and the training
    // binding (b3). It writes training records only. The Training pipeline must be ready.
    void ExecuteRadianceCacheTraining(
        ID3D12GraphicsCommandList4* commandList,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS trainingConstants,
        std::uint32_t threads);

    bool IsRadianceCacheLearningReady(PathTraceRadianceCacheLibrary library = PathTraceRadianceCacheLibrary::Inline) const
    {
        return m_radianceCache[static_cast<size_t>(library)].ready;
    }

    // Diagnostics (SHARC.4D-P/4D-Q): the driver's stack size of the Raster Primary ray generation, in the production
    // or a learning pipeline (0 if that pipeline does not exist).
    std::uint64_t GetRasterRayGenerationStackSize(bool learning,
        PathTraceRadianceCacheLibrary library = PathTraceRadianceCacheLibrary::Inline) const
    {
        const RayTracingPipeline& pipeline = learning ? m_radianceCache[static_cast<size_t>(library)].pipeline :
            m_pipelines[static_cast<size_t>(PathTracePrimary::Raster)];
        return pipeline.GetShaderStackSize(learning && library == PathTraceRadianceCacheLibrary::Training ?
            L"PathTraceRadianceCacheTrainingRayGeneration" : L"PathTraceRasterPrimaryRayGeneration");
    }

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
    // (constantsAddress names their UAVs too); without them nothing is recorded. NRD.RELAX.2B: rawDiffuseHitDistance
    // selects the raw-distance indirect pass as Execute does; the direct pass has no continuation and is unchanged.
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
        bool extract,
        D3D12_GPU_VIRTUAL_ADDRESS radianceCacheConstants = 0,
        PathTraceRadianceCacheLibrary radianceCacheLibrary = PathTraceRadianceCacheLibrary::RenderSplitIndirect,
        bool rawDiffuseHitDistance = false);

    // NRD.RELAX.2B: whether the last Execute or ExecuteReSTIRDISplit recorded a raw-distance extraction, which wrote the
    // raw diffuse hit distance texture. Only then may that frame's texture be read.
    bool WroteRawDiffuseHitDistance() const
    {
        return m_rawDiffuseHitDistanceWritten;
    }

    // NRD RELAX R.4 (diagnostics only): whether the last Execute or ExecuteReSTIRDISplit recorded the extraction direct
    // pass, which wrote every pixel's and sample's slot of the split direct diffuse buffer. Only then may that frame's
    // buffer be read, through its UAV (raw, float3 per pixel and sample), on the same command list.
    bool WroteSplitDirectDiffuse() const
    {
        return m_splitDirectDiffuseWritten;
    }

    unsigned int GetSplitDirectDiffuseUAVIndex() const
    {
        return m_splitDirectDiffuse.buffer.GetUAVDescriptorIndex();
    }

    std::uint32_t GetSplitDirectDiffuseSamples() const
    {
        return m_splitDirectDiffuse.samples;
    }

    // RTXPT D.1B.2 diffuse/specular signal extraction (diagnostics only, never enabled by default). Creates, on first
    // use, the extraction pipelines of Raster Primary or split ReSTIR DI (the extraction exports of the libraries
    // the production pipelines use) and the two RGBA16F outputs at the current extent. A failure is sticky until a
    // new revision of the libraries is accepted. Creation only: no GPU work is recorded.
    // NRD.RELAX.2B: rawDiffuseHitDistance additionally creates the raw-distance extraction pipeline of the primary (the
    // same export from PathTraceReferenceRawDiffuseHitDistance*.hlsl; split ReSTIR DI keeps the legacy extraction direct
    // pass). The legacy extraction pipelines are created first and stay valid if it fails.
    bool EnsureSignalExtraction(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap,
        PathTracePrimary primary,
        std::string& failureReason,
        bool rawDiffuseHitDistance = false);

    // The extraction outputs: diffuse RGB; specular RGB and the primary specular hit distance in alpha.
    Texture& GetDiffuseTexture()
    {
        return m_diffuseTexture;
    }

    Texture& GetSpecularTexture()
    {
        return m_specularTexture;
    }

    // SHARC.6A (experimental): the history prior the SHARC render library's extraction frames write (RGB the prior
    // in the diffuse signal's units, alpha its weight or negated absence code), created on first use at the extent.
    Texture* EnsureHistoryPriorTexture(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    Texture& GetHistoryPriorTexture()
    {
        return m_historyPriorTexture;
    }

    // NRD.RELAX.2 (diagnostics only): the R32F raw primary diffuse hit distance an extraction frame writes when its
    // constants carry this texture's UAV (PathTraceReferenceConstants::rawDiffuseHitDistanceIndex), created on first use
    // at the extent and released with the extraction outputs. The caller transitions it around the extraction.
    Texture* EnsureRawDiffuseHitDistanceTexture(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    Texture& GetRawDiffuseHitDistanceTexture()
    {
        return m_rawDiffuseHitDistanceTexture;
    }

    // NRD.RELAX.2B: whether the texture, a raw-distance extraction pipeline or a raw SHARC render pipeline exists.
    bool HasRawDiffuseHitDistanceResources() const;

    // Releases the texture and every raw-distance pipeline (NRD.RELAX.2B), clearing their failures; the next request
    // creates them again from the cached libraries. Caller guarantees no submitted work can still use them (an idle
    // queue, or, for ER.1 Phase 3.1 eviction, a completed last-use fence).
    void ReleaseRawDiffuseHitDistanceResources();

    // Copies the presentation image into the HDR scene colour consumed by tone mapping. Both are
    // RGBA16F at the scene extent; the caller checks that before recording.
    void CopyToSceneColor(ID3D12GraphicsCommandList* commandList, RenderTarget& sceneColor);

#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    bool WroteSharcDiagnostic() const { return m_sharcDiagnosticWritten; }
#endif

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

    bool IsSecondaryShadowRayQueryReady() const;
    void SelectSecondaryShadowRayQuery(bool enabled);

    bool IsPrimaryDiShadowRayQueryReady() const;
    void SelectPrimaryDiShadowRayQuery(bool enabled);

    // Both direct entries are prepared synchronously with the split pipelines, so not ready afterwards is a failure
    // (logged as "DI.1 ..."), not unfinished preparation.
    bool IsPrimaryDiShadowRayQueryFailed() const
    {
        return m_initialized && m_splitPipelinesReady && !IsPrimaryDiShadowRayQueryReady();
    }

    bool IsPrimaryDiShadowRayQueryActive() const
    {
        return m_primaryDiShadowRayQuery;
    }

    bool IsSecondaryShadowRayQueryActive() const
    {
        return m_secondaryShadowRayQuery;
    }

    private:
    bool InitializeVisibilityPipelines(GraphicsDevice& device, ShaderLibrary& shaders,
        RayTracingPipeline& original, const std::vector<char>& bytecode,
        const RayTracingPipelineDescription& description);
    RayTracingPipeline& VisibilityPipeline(RayTracingPipeline& original);
    RayTracingPipeline& PrimaryDiVisibilityPipeline(RayTracingPipeline& original);
    bool PreparePrimaryDiExtractionPipeline(GraphicsDevice& device, ShaderLibrary& shaders);

    // Retained alongside the originals until the caller's existing idle teardown boundary.
    std::map<RayTracingPipeline*, std::unique_ptr<RayTracingPipeline>> m_rayQueryPipelines;
    bool m_secondaryShadowRayQuery = false;
    std::map<RayTracingPipeline*, std::unique_ptr<RayTracingPipeline>> m_primaryDiRayQueryPipelines;
    bool m_primaryDiShadowRayQuery = false;
    bool m_primaryDiExtractionPipelineReady = false;

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
        // ER.1 Phase 4: changes whenever library is replaced; a prepared pipeline is adopted only at its revision.
        std::uint64_t revision = 0;
        // ER.1 Phase 4: an immutable copy of library.bytecode shared with background pipeline requests.
        std::shared_ptr<const std::vector<char>> snapshot;
    };

    // ER.1 Phase 4: a pipeline the worker built, owned here until a creation path adopts it.
    struct PreparedPipeline
    {
        std::uint64_t revision = 0;
        ID3D12Device5* device = nullptr;
        std::unique_ptr<RayTracingPipeline> pipeline;
    };

    std::uint64_t LibraryRevision(const std::filesystem::path& path) const;
    // The slot's pipeline: the prepared one when its key, revision and device match; otherwise created here (a
    // configuration no gate prepared: diagnostics, packaged runtimes, a library edited after preparation). A pipeline
    // whose background creation failed at the current revision is not retried synchronously.
    bool CreatePipeline(GraphicsDevice& device, RayTracingPipeline& slot, const std::vector<char>& bytecode,
        const RayTracingPipelineDescription& description);
    bool IsPipelineLive(const std::string& identity) const;

    std::uint64_t m_libraryRevisionCounter = 0;
    std::map<std::string, PreparedPipeline> m_preparedPipelines;
    // Description key -> the library revision whose background creation failed.
    std::map<std::string, std::uint64_t> m_failedPipelines;
    PipelineStatistics m_pipelineStatistics;
    std::string m_pipelineFailureForTesting;

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
    // NRD.RELAX.2B raw-distance extraction pipelines, created only when requested (EnsureSignalExtraction).
    RayTracingPipeline m_extractRasterRawPipeline;
    RayTracingPipeline m_extractSplitIndirectRawPipeline;
    bool m_extractRasterRawReady = false;
    bool m_extractRasterRawFailed = false;
    bool m_extractSplitRawReady = false;
    bool m_extractSplitRawFailed = false;
    std::string m_extractRasterRawFailure;
    std::string m_extractSplitRawFailure;
    bool m_rawDiffuseHitDistanceWritten = false;
    bool m_splitDirectDiffuseWritten = false;
#if PATH_TRACE_SHARC_SPATIAL_DIAGNOSTIC != 0
    bool m_sharcDiagnosticWritten = false;
#endif
    Texture m_diffuseTexture;
    Texture m_specularTexture;
    Texture m_historyPriorTexture;
    Texture m_rawDiffuseHitDistanceTexture;

    // Creates the extraction outputs at the current extent when they do not exist.
    bool CreateExtractionTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    // Creates the legacy or (NRD.RELAX.2B) raw-distance extraction pipelines of Raster Primary or split ReSTIR DI when
    // missing. A failure releases every pipeline of that set and is sticky until a new library revision is accepted.
    bool EnsureExtractionPipelines(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        bool split,
        bool rawDiffuseHitDistance,
        std::string& failureReason);

    // SHARC.4D learning pipelines by PathTraceRadianceCacheLibrary, each created on first use
    // (EnsureRadianceCacheLearning).
    struct RadianceCacheLearningPipeline
    {
        RayTracingPipeline pipeline;
        bool ready = false;
        bool failed = false;
        std::string failure;
    };

    RadianceCacheLearningPipeline m_radianceCache[static_cast<size_t>(PathTraceRadianceCacheLibrary::Count)];

    // The UAV barriers after a frame's last dispatch: accumulation and statistics, plus the extraction outputs.
    void RecordOutputBarriers(ID3D12GraphicsCommandList* commandList, bool extract);

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
    PathTraceLibraryVariant m_libraryVariant = PathTraceLibraryVariant::SM66;
};
