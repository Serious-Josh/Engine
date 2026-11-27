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
#include <set>

class ComputePipeline;
class DescriptorHeap;
class GraphicsDevice;
class RenderTarget;
class ShaderLibrary;
class RayTracingSceneResources;

// SER S6B: the DXR library set every path-tracing primary uses, chosen once at startup.
//   SM66    PathTraceReference.hlsl (lib_6_6): the conventional path.
//   SM69    PathTraceReferenceSM69.hlsl: diagnostic only, the bit-exact non-SER baseline of SM69SER.
//   SM69SER PathTraceReferenceSERSM69.hlsl: shader execution reordering on the continuation rays of Reference, Raster
//           Primary and ReSTIR DI; Pixel Centre traces conventionally.
// The ReSTIR DI direct pass is a compute shader (DirectLighting/, cs_6_6) in every variant.
enum class PathTraceLibraryVariant
{
    SM66,
    SM69,
    SM69SER
};

const char* PathTraceLibraryVariantName(PathTraceLibraryVariant variant);

// SHARC library slots of each variant (NVIDIA SHARC v1.8.3, NvidiaRadianceCacheLibraryPath). Training is the update
// library's training ray generation, dispatched by ExecuteRadianceCacheTraining. Render substitutes cached radiance;
// RenderExtraction is the same library's signal-extraction ray generation, for frames the path-tracing denoiser filters.
// RenderSplitIndirect[Extraction] are its ReSTIR DI indirect-pass exports, for ExecuteReSTIRDISplit. The
// *RawDiffuseHitDistance entries are the extraction exports of the raw-distance build. Psr* are the PSR.2 combined
// libraries. (The D6 cache's Deferred, DeferredLate, Query and QueryTermination slots were removed on 2026-11-24.)
// Which library set serves them: Nvidia, or NvidiaDepthMatched, NVIDIA SHARC's diagnostic update library without cache
// resampling (PathTraceReferencePass::SetRadianceCacheImplementation selects the set).
enum class PathTraceRadianceCacheImplementation
{
    Nvidia,
    NvidiaDepthMatched,
    Count
};

enum class PathTraceRadianceCacheLibrary
{
    Training,
    Render,
    RenderExtraction,
    RenderSplitIndirect,
    RenderSplitIndirectExtraction,
    RenderExtractionRawDiffuseHitDistance,
    RenderSplitIndirectExtractionRawDiffuseHitDistance,
    PsrTraining,
    PsrRenderSplitIndirect,
    PsrRenderSplitIndirectExtraction,
    PsrRenderSplitIndirectExtractionRawDiffuseHitDistance,
    Count
};

// ER.1 Phase 2: what a path-traced configuration records, for the DXR libraries it must have before activation.
struct PathTraceLibraryNeeds
{
    PathTracePrimary primary = PathTracePrimary::Camera;
    // ER.1 Phase 4: signal extraction (any path-tracing denoiser) records the extraction pipelines.
    // Zero prepares the ordinary path; 1/2 prepare only the compatible PSR lighting set.
    std::uint32_t psrChain = 0;
    bool extraction = false;
    // The raw diffuse hit distance extraction (NRD RELAX and the hybrid).
    bool rawDiffuseHitDistance = false;
    // SHARC: the sparse training library and the render library that substitutes cached radiance.
    bool radianceCache = false;
    PathTraceRadianceCacheLibrary radianceCacheRender = PathTraceRadianceCacheLibrary::Render;
    PathTraceRadianceCacheImplementation radianceCacheImplementation = PathTraceRadianceCacheImplementation::Nvidia;
    // Startup work (4.1): ReSTIR PT Enhanced dispatches none of the primary's DXR pipelines (its own compute kernels render
    // it); with SHARC it needs only the cache's training pipeline. Its frames use the primary's targets, not its pipelines.
    bool restirPt = false;
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
    // Read-only local regression instrumentation; no production execution path.
    friend class SceneHierarchyVerification;
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
    // M0c.3 Phase A: releases every render-extent texture and buffer (the ones Resize recreates, including the lazily
    // created extraction, split-sample and PSR resources) with their descriptors; pipelines and the statistics texture stay.
    // Resize creates the textures again; the lazy resources follow on their next use. The caller guarantees the GPU no
    // longer uses them.
    void ReleaseTextures();

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

    // Startup work (4.6, decision 3): a packaged game ships one shadow-ray flavour, RayQuery. With a bootstrap that cooked
    // the GI.6 twins without their TraceRay originals, every pipeline that has a twin is built from the twin, in the
    // original's slot (the libraries and pipelines prepared, acquired and created are the twins'), and no TraceRay pipeline
    // is created. A bootstrap that still holds the originals keeps both flavours, as before. Call after
    // SelectLibraryVariant, before anything is prepared.
    void SelectRayQueryOnly(const ShaderLibrary& shaderLibrary);

    bool IsRayQueryOnly() const
    {
        return m_rayQueryOnly;
    }

    // Startup work (4.6, decision 3, Editor): with the RayQuery setting on when the first configuration is requested, the
    // Editor builds twinned slots from their twins as a packaged game does, and compiles no TraceRay original. Call before
    // anything is prepared; ignored for a packaged game (SelectRayQueryOnly decides there).
    void SetRayQueryFirst(bool enabled, const ShaderLibrary& shaderLibrary);
    // A twin library failed to compile in the Editor: slots built from now on use the TraceRay originals (with their twins
    // optional, as before); slots already built keep RayQuery. Returns false when nothing changed.
    bool AbandonRayQueryFirst();
    static bool IsRayQueryTwinLibrary(const std::filesystem::path& path);

    // The slots built from a twin, whose TraceRay pipeline exists only once PrepareTraceRayFallbacks made it.
    bool HasRayQuerySlots() const
    {
        return !m_rayQuerySlots.empty();
    }

    // Startup work (4.6): the TraceRay pipelines of every RayQuery slot, prepared on first use (the RayQuery setting turned
    // off in the Editor): each original library on the compile worker, then its pipeline on the pipeline worker (or here,
    // without one). Never blocks while either worker has it. Ready once every slot has its TraceRay pipeline; until then,
    // and if any fails, the slots keep rendering RayQuery (VisibilityPipeline uses the TraceRay set only whole).
    ShaderLibraryPreparation PrepareTraceRayFallbacks(GraphicsDevice& device, ShaderLibrary& shaderLibrary,
        ShaderLibraryCompileWorker* worker, RayTracingPipelineWorker* pipelineWorker);

    // Creates the pipeline of a primary on first use (startup work 4.1: Initialize creates the targets only, so the Camera
    // pipeline too is created here, by the Reference mode alone). A failure, for example a cooked library from before
    // RTXPT.2 R3 without the export, is sticky for this pass lifetime and leaves the other primaries working.
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

    // PS.5B: the SM 6.6 libraries packaged games cook. GetRequiredLibraries for Reference and Path Tracing (Raster
    // Primary + ReSTIR DI) with every path-tracing denoiser's extraction; no PSR or SHARC.
    static std::vector<PathTraceLibraryRequirement> GetPackagedLibraries();

    // ER.1 Phase 4: every DXR pipeline the configuration's creation paths build (the same description builders), with
    // the RayQuery twin InitializeVisibilityPipelines pairs with each. Optional entries are twins.
    struct PipelineRequirement
    {
        RayTracingPipelineDescription description;
        bool optional = false;
    };

    std::vector<PipelineRequirement> GetRequiredPipelines(const PathTraceLibraryNeeds& needs) const;

    // M0c.4 Stage 4.0 readiness probe: the state PrepareLibrary / PreparePipelines would report, observed without taking
    // worker results, submitting, accepting or logging. A library is Ready once this pass accepted it; a pipeline once it
    // is live or prepared for adoption at the current revision (a failed optional twin counts as Ready, as there).
    // `detail` says what is pending or failed.
    ShaderLibraryPreparationState ObserveLibrary(const ShaderLibraryCompileWorker* worker, bool packaged,
        const std::filesystem::path& path, std::string& detail) const;
    ShaderLibraryPreparationState ObservePipeline(const RayTracingPipelineWorker* worker, ID3D12Device5* device,
        const PipelineRequirement& requirement, std::string& detail) const;
    // The key a pipeline's description is prepared under (identifies one pipeline across configurations).
    static std::string PipelineKey(const RayTracingPipelineDescription& description);

    // ER.1 Phase 4: polls the pipeline worker for every required pipeline, never blocking. Requires the configuration's
    // libraries accepted (PrepareLibrary Ready). Ready once each pipeline is live in its slot or prepared for adoption at
    // the current library revision; PreparingPipelines while any is queued or creating; Failed when a required one
    // failed (a failed twin counts as Ready: its pipeline keeps TraceRay). The creation paths then adopt the prepared
    // pipelines (RayTracingPipeline::Swap) instead of calling CreateStateObject on the render thread.
    // Startup work (4.5): with acceptedLibraries, only the pipelines of those libraries are polled and queued (the others
    // are left for a later call and do not affect the result), so a configuration's pipelines are created as each library
    // lands rather than after its last one. background marks the jobs as a speculative warm-up
    // (RayTracingPipelineRequest::background).
    ShaderLibraryPreparation PreparePipelines(RayTracingPipelineWorker& worker, ID3D12Device5* device,
        const PathTraceLibraryNeeds& needs, const std::vector<std::filesystem::path>* acceptedLibraries = nullptr,
        bool background = false);

    struct PipelineStatistics
    {
        std::uint64_t adopted = 0;
        std::uint64_t synchronous = 0;
        std::uint64_t submitted = 0;
        std::uint64_t failures = 0;
        std::uint64_t discarded = 0;
        // M4.2C: sampled-NEE permutations built.
        std::uint64_t sampledVariants = 0;
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
        PathTraceRadianceCacheLibrary radianceCacheLibrary = PathTraceRadianceCacheLibrary::Render,
        bool rawDiffuseHitDistance = false,
        bool primarySurfaceReplacement = false,
        GpuTimestampProfiler* profiler = nullptr,
        unsigned int frameIndex = 0,
        D3D12_GPU_VIRTUAL_ADDRESS guideConstants = 0,
        D3D12_GPU_VIRTUAL_ADDRESS guideTransforms = 0);

    // PSR.1b (development, default off): creates, on first use, the guide pipeline (the record pass plus virtual guides,
    // with its own hit group and b3/t1 bindings) and the 32-byte-per-pixel guide buffer. Returns the guide buffer's raw
    // UAV index (PathTracePsrGuideConstants::guideIndex), or UINT_MAX. Execute with nonzero guide inputs on a PSR frame
    // then records the guide pass in place of the record pass.
    unsigned int EnsurePrimarySurfaceReplacementGuides(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap,
        std::string& failureReason);

    bool WrotePrimarySurfaceReplacementGuides() const
    {
        return m_psrGuidesWritten;
    }

    // Diagnostics only: the guides of the last guide pass (no resource before the first).
    Buffer& GetPrimarySurfaceReplacementGuides()
    {
        return m_psrGuides;
    }

    // PSR.1a (development, default off): creates, on first use, the PSR library's record and Raster Primary pipelines
    // and the per-pixel record buffer at the current extent. Returns the record buffer's raw UAV index
    // (PathTraceReferenceConstants::psrRecordIndex), or UINT_MAX when anything is unavailable; a pipeline failure is
    // sticky. Execute with primarySurfaceReplacement then records the replacement dispatch and the PSR raster primary in
    // place of the production one, for Raster Primary without extraction or a SHARC library only.
    unsigned int EnsurePrimarySurfaceReplacement(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        DescriptorHeap& descriptorHeap,
        std::string& failureReason);

    // Whether the last Execute recorded the replacement (diagnostic metadata only).
    bool WrotePrimarySurfaceReplacement() const
    {
        return m_psrWritten;
    }

    // Diagnostics only: discards the PSR GI.6 twins, as a failed twin build would leave them.
    void DropPrimarySurfaceReplacementRayQueryForTesting()
    {
        for(auto* twins : {&m_psrRayQueryPipelines})
        {
            for(auto& [original, alternate] : *twins)
            {
                if(alternate)
                {
                    alternate->Shutdown();
                    alternate.reset();
                }
            }
        }
    }

    // Diagnostics only: the records of the last replacement dispatch (no resource before the first).
    Buffer& GetPrimarySurfaceReplacementRecords()
    {
        return m_psrRecords;
    }

    // Development-only preparation: all downstream state objects before in-place replacement.
    bool EnsurePsrSharcInfrastructure(GraphicsDevice&, ShaderLibrary&, bool extract, bool raw, std::string&);

    // Multibounce PSR: the chain length the PSR.2 lighting libraries handle, 1 or 2 (their PsrK2/ variants, built with
    // PATH_TRACE_PSR_CHAIN 2). A change parks the live pipelines for later adoption. Identified production
    // pipelines stay alive until GPU-confirmed shutdown, so switching requires no GPU wait. Returns whether it changed.
    bool SetPrimarySurfaceReplacementChain(std::uint32_t chain);
    std::uint32_t GetPrimarySurfaceReplacementChain() const
    {
        return m_psrChain;
    }
    // Verification only: the directory of the two-bounce libraries in place of PsrK2/ (empty: PsrK2/), with the same
    // release rules as SetPrimarySurfaceReplacementChain.
    void SetPrimarySurfaceReplacementChainDirectoryForTesting(const std::filesystem::path& directory);
    bool PsrSharcInfrastructureReady(bool extract, bool raw);

    // SHARC: creates, on first use, the pipeline of a SHARC library slot (PathTraceRadianceCacheLibrary, with the cache
    // binding at b3) for the selected implementation. A failure is sticky until a new revision of the libraries is
    // accepted. Creation only.
    bool EnsureRadianceCacheLearning(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        std::string& failureReason,
        PathTraceRadianceCacheLibrary library);

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
        std::uint32_t threads,
        D3D12_GPU_VIRTUAL_ADDRESS psrConstants = 0,
        D3D12_GPU_VIRTUAL_ADDRESS psrTransforms = 0);

    bool IsRadianceCacheLearningReady(PathTraceRadianceCacheLibrary library) const
    {
        return m_radianceCache[static_cast<size_t>(library)].ready;
    }

    // The cache whose libraries EnsureRadianceCacheLearning, ExecuteRadianceCacheTraining and the render paths use.
    void SetRadianceCacheImplementation(PathTraceRadianceCacheImplementation implementation)
    {
        m_radianceCacheImplementation = implementation;
        m_radianceCache = m_radianceCacheSets[static_cast<size_t>(implementation)];
    }

    PathTraceRadianceCacheImplementation GetRadianceCacheImplementation() const
    {
        return m_radianceCacheImplementation;
    }

    // Diagnostics (SHARC.4D-P/4D-Q): the driver's stack size of the Raster Primary ray generation, in the production
    // or a learning pipeline (0 if that pipeline does not exist).
    std::uint64_t GetRasterRayGenerationStackSize(bool learning,
        PathTraceRadianceCacheLibrary library = PathTraceRadianceCacheLibrary::Render) const
    {
        const RayTracingPipeline& pipeline = learning ? m_radianceCache[static_cast<size_t>(library)].pipeline :
            m_pipelines[static_cast<size_t>(PathTracePrimary::Raster)];
        return pipeline.GetShaderStackSize(learning && library == PathTraceRadianceCacheLibrary::Training ?
            L"PathTraceSharcNvTrainingRayGeneration" : L"PathTraceRasterPrimaryRayGeneration");
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
        bool rawDiffuseHitDistance = false,
        D3D12_GPU_VIRTUAL_ADDRESS psrGBufferConstants = 0,
        D3D12_GPU_VIRTUAL_ADDRESS psrGBufferTransforms = 0);

    // M5.1 ReSTIR PT Enhanced (configuration B): records the unified initial path sampling in the split passes' place. The
    // pipeline (ReSTIRPTPass::GetInitialPipeline) has this pass's root signature (TLAS, accumulation, output, statistics,
    // b0, b1) plus RestirPtConstants at b2, and writes this pass's accumulation, output and statistics and, with extract,
    // its diffuse and specular extraction textures (and, with rawDiffuseHitDistance, the raw diffuse hit distance), so
    // every consumer of those reads configuration B's frame unchanged. Timed in RestirPtInitial. Records nothing when the
    // extent differs or extraction is requested without its textures.
    void ExecuteReSTIRPT(
        ID3D12GraphicsCommandList* commandList,
        GpuTimestampProfiler& profiler,
        unsigned int frameIndex,
        DescriptorHeap& descriptorHeap,
        const RayTracingSceneResources& rayTracingScene,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS sceneLightConstantsAddress,
        unsigned int width,
        unsigned int height,
        const ComputePipeline& pipeline,
        D3D12_GPU_VIRTUAL_ADDRESS restirPtConstantsAddress,
        D3D12_GPU_VIRTUAL_ADDRESS restirPtTransformsAddress,
        bool extract,
        bool rawDiffuseHitDistance,
        // M5.3: the spatial pre-pass and resample pipelines (same bindings), dispatched after the initial pass when given.
        const ComputePipeline* spatialPairs = nullptr,
        const ComputePipeline* spatialResample = nullptr,
        // M5.4 C1: the temporal reuse pass (same bindings), dispatched between the initial pass and spatial reuse when given.
        const ComputePipeline* temporalPass = nullptr,
        // M5.4 C5: the replay dispatch's constants (replay compaction); 0 for none.
        D3D12_GPU_VIRTUAL_ADDRESS replayConstantsAddress = 0,
        // M5.4 C7: the duplication map pass, after the final reservoirs; null for none.
        const ComputePipeline* duplication = nullptr);

    // PSR.2 Stage 3A (development, default off): creates, on first use, the PSR.2 lighting pair, the PSR.1c-A split
    // pair built with G-buffer storage (PATH_TRACE_PSR_GBUFFER) and its DI.1/GI.6 twins. A failure is sticky.
    // ExecuteReSTIRDISplit with both PSR.2 addresses (PathTracePsrGBufferConstants at b4, the replacement pass's object
    // transforms at t2) then records this pair in place of the production pair and of PSR.1's, without extraction or a
    // SHARC library only.
    bool EnsurePrimarySurfaceReplacementGBuffer(
        GraphicsDevice& graphicsDevice,
        ShaderLibrary& shaderLibrary,
        std::string& failureReason);
    // Whether the pair and the twins the current visibility selections need exist.
    bool IsPrimarySurfaceReplacementGBufferReady();
    // PS.5C: the PSR.2 lighting pipelines exist (IsPrimarySurfaceReplacementGBufferReady also needs their RayQuery twins
    // while the frame selects RayQuery visibility).
    bool ArePrimarySurfaceReplacementGBufferPipelinesCreated() const { return m_psrGBufferPipelinesReady; }
    // PSR.2 Stage 3B: the pair's extraction counterparts, on first use: the extraction direct pass and the extraction
    // (or, with rawDiffuseHitDistance, raw-distance extraction) indirect pass, each split at the effective primary. A
    // failure is sticky per kind. ExecuteReSTIRDISplit with extraction then records them in place of the production
    // extraction pair.
    bool EnsurePrimarySurfaceReplacementGBufferExtraction(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary,
        bool rawDiffuseHitDistance, std::string& failureReason);
    bool IsPrimarySurfaceReplacementGBufferExtractionReady(bool rawDiffuseHitDistance);

    // Whether the last ExecuteReSTIRDISplit recorded the PSR.2 lighting pair.
    bool WrotePrimarySurfaceReplacementGBuffer() const
    {
        return m_psrGBufferWritten;
    }

    // PSR.2 Stage 3A verification only: with a probe buffer (192 bytes per pixel, PathTracePsrProbeRayGeneration) and
    // its pipelines, ExecuteReSTIRDISplit dispatches the probe of the PSR pair it recorded (PSR.1's or PSR.2's) after the
    // indirect pass. The renderer passes PsrProbeUAVIndex in each library's spare constant word. Null in normal rendering.
    bool EnsurePsrProbePipelinesForTesting(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);
    void SetPsrProbeForTesting(Buffer* probe)
    {
        m_psrProbeForTesting = probe;
    }
    std::uint32_t PsrProbeUAVIndex() const
    {
        return m_psrProbeForTesting ? m_psrProbeForTesting->GetUAVDescriptorIndex() : 0u;
    }
    // Multibounce PSR M2-A verification only: the chain probe built from a diagnostic library that includes the PSR.2
    // indirect pass's production library (raygeneration PsrChainProbeRayGeneration). With the probe buffer set it replaces
    // the PSR.2 probe after the indirect pass, with that pass's bindings. Uninitialized in normal rendering.
    bool EnsurePsrChainProbePipelinesForTesting(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary,
        const std::filesystem::path& indirectLibrary);

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

    // Startup work (4.1): the extraction outputs alone, for ReSTIR PT Enhanced, whose kernels write them (it records none of
    // the extraction pipelines). False when the pass is not initialized or they cannot be allocated.
    bool EnsureSignalExtractionTargets(ID3D12Device* device, DescriptorHeap& descriptorHeap);

    // The extraction outputs: diffuse RGB; specular RGB and the primary specular hit distance in alpha.
    Texture& GetDiffuseTexture()
    {
        return m_diffuseTexture;
    }

    Texture& GetSpecularTexture()
    {
        return m_specularTexture;
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

    bool IsSecondaryShadowRayQueryActive() const
    {
        return m_secondaryShadowRayQuery;
    }

    // M4.2A/M4.2C: ENGINE_SAMPLED_NEE=1 (read once, unpackaged runs) also builds each RayQuery twin's and each NVIDIA
    // SHARC update library's separate sampled-NEE permutation (the M42A libraries). Off by default; production libraries
    // never contain the sampled estimator.
    static bool SampledIndirectNeeLibrariesRequested();
    // M4.3.5: the ReSTIR DI direct passes' ReGIR permutations (DirectLighting/ReGIR) are prepared by default; ENGINE_REGIR=0
    // disables ReGIR entirely (no permutations, no presampling: uniform local-light candidates as before M4.3).
    static bool ReGIRLibrariesRequested();
    // M4.3: whether the next recorded frame's direct pass uses its ReGIR permutation (when it has one), and whether the
    // last recorded frame did.
    void SelectReGIR(bool enabled)
    {
        m_regir = enabled;
    }

    bool RecordedReGIR() const
    {
        return m_regirRecorded;
    }

    // M4.2C: whether this frame's image paths and SHARC training use their sampled permutations (set by the renderer
    // together with the SceneLightConstants mode bits, before recording). A pipeline without one runs the exhaustive
    // permutation, which falls back to the same estimate in expectation.
    void SelectSampledNee(bool image, bool training)
    {
        m_sampledImage = image;
        m_sampledTraining = training;
        m_sampledImageRecorded = false;
        m_sampledTrainingRecorded = false;
    }

    // M4.2C: whether the last recorded image path / SHARC training dispatch used a sampled permutation.
    bool RecordedSampledImage() const
    {
        return m_sampledImageRecorded;
    }

    bool RecordedSampledTraining() const
    {
        return m_sampledTrainingRecorded;
    }

    private:
    bool InitializeVisibilityPipelines(GraphicsDevice& device, ShaderLibrary& shaders,
        RayTracingPipeline& original, const std::vector<char>& bytecode,
        const RayTracingPipelineDescription& description);
    // M4.2C: builds owner's sampled permutation from description's M42A library, or clears it.
    void AttachSampledVariant(GraphicsDevice& device, ShaderLibrary& shaders, RayTracingPipeline& owner,
        const RayTracingPipelineDescription& description);
    // M4.3: builds owner's ReGIR permutation from description's DirectLighting/ReGIR library, or clears it.
    void AttachReGIRVariant(GraphicsDevice& device, ShaderLibrary& shaders, RayTracingPipeline& owner,
        const RayTracingPipelineDescription& description);
    RayTracingPipeline& VisibilityPipeline(RayTracingPipeline& original);
    bool m_regir = false;
    bool m_regirRecorded = false;
    bool m_sampledImage = false;
    bool m_sampledTraining = false;
    bool m_sampledImageRecorded = false;
    bool m_sampledTrainingRecorded = false;
    // Prepares the compute extraction direct pass (m_extractSplitDirectPipeline).
    bool PreparePrimaryDiExtractionPipeline(GraphicsDevice& device, ShaderLibrary& shaders);

    // Retained alongside the originals until the caller's existing idle teardown boundary.
    std::map<RayTracingPipeline*, std::unique_ptr<RayTracingPipeline>> m_rayQueryPipelines;
    bool m_secondaryShadowRayQuery = false;
    // Startup work (4.6): SelectRayQueryOnly; and the library a slot is built from (the twin's when RayQuery-only).
    bool m_rayQueryOnly = false;
    std::filesystem::path SlotLibraryPath(const std::filesystem::path& path) const;
    // Startup work (4.6): each slot built from its twin, with the TraceRay description it would otherwise have used, and
    // the TraceRay pipelines PrepareTraceRayFallbacks made for them (used only when all are ready and RayQuery is off).
    std::map<RayTracingPipeline*, RayTracingPipelineDescription> m_rayQuerySlots;
    std::map<RayTracingPipeline*, std::unique_ptr<RayTracingPipeline>> m_traceRayPipelines;
    bool m_traceRayFallbacksReady = false;
    bool m_traceRayFallbacksFailed = false;
    void ForgetRayQuerySlot(RayTracingPipeline& slot);
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
    void RetainPsrPipeline(RayTracingPipeline& pipeline);
    std::map<std::string, std::unique_ptr<RayTracingPipeline>> m_retainedPsrPipelines;
    std::vector<std::unique_ptr<RayTracingPipeline>> m_retiredPsrDuplicates;

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

    // PS.5B.1: PrepareLibrary in a packaged game. The cooked library is admitted (ShaderLibrary validates its
    // container) and is then Ready like a compiled one; a failure is final for the session and reported once,
    // a missing optional twin as the expected TraceRay fallback, anything else as an error.
    ShaderLibraryPreparation PreparePackagedLibrary(
        ShaderLibrary& shaderLibrary,
        const std::filesystem::path& path,
        bool optional);
    void ReportMissingPackagedTwin(const std::filesystem::path& path);

    // Every primary shares PathTraceReference.hlsl except ReSTIR DI, so this holds at most two libraries
    // until Shutdown. Pipelines keep their own copies inside their state objects.
    std::vector<CompiledLibrary> m_libraries;
    std::vector<FailedLibrary> m_failedLibraries;
    std::map<std::filesystem::path, std::string> m_packagedLibraryFailures;
    std::set<std::string> m_reportedMissingTwins;

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

    RadianceCacheLearningPipeline m_radianceCacheSets[static_cast<size_t>(PathTraceRadianceCacheImplementation::Count)]
        [static_cast<size_t>(PathTraceRadianceCacheLibrary::Count)];
    // The selected implementation's set (SetRadianceCacheImplementation).
    RadianceCacheLearningPipeline* m_radianceCache = m_radianceCacheSets[0];
    PathTraceRadianceCacheImplementation m_radianceCacheImplementation = PathTraceRadianceCacheImplementation::Nvidia;

    // PSR.1a, created on first use (EnsurePrimarySurfaceReplacement): the record pass and the raster primary of the PSR
    // library, and 48 bytes per pixel of records (PATH_TRACE_PSR_RECORD_STRIDE), recreated with the extent.
    RayTracingPipeline m_psrRecordPipeline;
    RayTracingPipeline m_psrRasterPipeline;
    bool m_psrPipelinesReady = false;
    bool m_psrPipelinesFailed = false;
    std::string m_psrFailure;
    Buffer m_psrRecords;
    bool m_psrWritten = false;

    // PSR.1b, created on first use (EnsurePrimarySurfaceReplacementGuides): the guide pass and 32 bytes per pixel of
    // guides (PATH_TRACE_PSR_GUIDE_STRIDE), recreated with the extent.
    RayTracingPipeline m_psrGuidePipeline;
    bool m_psrGuidePipelineReady = false;
    bool m_psrGuidePipelineFailed = false;
    std::string m_psrGuideFailure;
    Buffer m_psrGuides;
    bool m_psrGuidesWritten = false;

    // PSR.1a-P: the GI.6 RayQuery twins of the two PSR pipelines, kept apart from m_rayQueryPipelines so their
    // readiness never gates IsSecondaryShadowRayQueryReady.
    std::map<RayTracingPipeline*, std::unique_ptr<RayTracingPipeline>> m_psrRayQueryPipelines;

    // PSR.2 Stage 3A: the G-buffer-storage split pair (EnsurePrimarySurfaceReplacementGBuffer); twins in the PSR maps.
    RayTracingPipeline m_psrGBufferDirectPipeline;
    RayTracingPipeline m_psrGBufferIndirectPipeline;
    bool m_psrGBufferPipelinesReady = false;
    bool m_psrGBufferPipelinesFailed = false;
    std::string m_psrGBufferFailure;
    // Multibounce PSR (SetPrimarySurfaceReplacementChain).
    std::uint32_t m_psrChain = 1;
    // RayQuery 2M E3: the ReSTIR DI direct pass's pipeline description for a PSR chain (0: PSR off) and NRD signal
    // extraction: the DirectLighting/ compute shader. It keeps the DXR split description's root signature (so the pass
    // binds the same root arguments) and checks its hit groups against the compute shader's inline radiance ray.
    RayTracingPipelineDescription DirectPassDescription(bool extraction, std::uint32_t psrChain) const;
    std::filesystem::path m_psrChainDirectoryForTesting;
    // A PSR lighting library's path for m_psrChain: PsrK2/<path> for two-mirror chains.
    std::filesystem::path PsrChainLibraryPath(const std::filesystem::path& path) const
    {
        if(m_psrChain < 2)
        {
            return path;
        }

        return (m_psrChainDirectoryForTesting.empty() ? std::filesystem::path("PsrK2") : m_psrChainDirectoryForTesting) /
            path;
    }
    // Releases every PSR lighting pipeline and its failure, including the combined PSR + SHARC learning pipelines.
    void ResetPrimarySurfaceReplacementChainPipelines();
    bool m_psrGBufferWritten = false;
    // PSR.2 Stage 3B: extraction direct, extraction indirect and raw-distance extraction indirect.
    RayTracingPipeline m_psrGBufferExtractDirectPipeline;
    RayTracingPipeline m_psrGBufferExtractIndirectPipeline;
    RayTracingPipeline m_psrGBufferExtractIndirectRawPipeline;
    bool m_psrGBufferExtractReady[2] = {};
    bool m_psrGBufferExtractFailed[2] = {};
    std::string m_psrGBufferExtractFailure[2];
    // Verification only: the probe buffer and the probe pipelines of the PSR.1 and PSR.2 libraries.
    Buffer* m_psrProbeForTesting = nullptr;
    RayTracingPipeline m_psrProbePipelines[2];
    // Verification only (M2-A): the chain probe of the PSR.2 indirect library.
    RayTracingPipeline m_psrChainProbePipeline;

    // Shuts down both PSR pipelines and their GI.6 twins. Caller guarantees the queue is idle.
    void ReleasePrimarySurfaceReplacementPipelines();

    // The UAV barriers after a frame's last dispatch: accumulation and statistics, plus the extraction outputs.
    void RecordOutputBarriers(ID3D12GraphicsCommandList* commandList, bool extract);

    unsigned int m_width = 0;
    unsigned int m_height = 0;
    bool m_initialized = false;
    PathTraceLibraryVariant m_libraryVariant = PathTraceLibraryVariant::SM66;
};
