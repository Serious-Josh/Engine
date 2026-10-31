#pragma once

#include <d3d12.h>
#include <cstdint>

class GpuTimestampProfiler
{
    public:
    static constexpr unsigned int FrameCount = 2;

    enum class Region : unsigned int
    {
        TotalFrame,
        Geometry,
        RayTracedShadows,
        ShadowDenoise,
        RayTracedReflections,
        ReflectionContributionLimiter,
        NrdPrepare,
        NrdReblurSpecular,
        NrdResolve,
        DeferredLighting,
        ToneMap,
        TlasUpdate,
        SelectionMask,
        RayTracedAO,
        AOTemporal,
        AODenoise,
        RayTracedGI,
        ReflectionTemporal,
        ReflectionDenoise,
        NrdDiffuseResolve,
        DebugUI,
        GISpatialDenoise,
        NrdReblurDiffuse,
        PathTraceReference,
        // RTXPT.3 ReSTIR DI: each split dispatch, nested inside PathTraceReference, which
        // also spans the barrier between them.
        PathTraceSplitDirect,
        PathTraceSplitIndirect,
        // RTXPT D.3B spatial filter, after (not inside) PathTraceReference.
        PathTraceSpatial,
        // SHARC.4D experimental D6 radiance cache: the generation clear (before PathTraceReference) and the resolve
        // (after it). Its sample insertion runs inside PathTraceReference.
        RadianceCacheGeneration,
        RadianceCacheResolve,
        // SHARC.4D-Q deferred insertion (CSInsertRecords), after PathTraceReference and before the resolve.
        RadianceCacheInsertion,
        // SHARC.4D-R sparse training dispatch, after PathTraceReference and before the insertion.
        RadianceCacheTraining,
        // UPSCALING.2C diagnostic presentation resolve (reduced render scale only), before ToneMap.
        PresentationResolve,
        // UPSCALING.3B DLSS input preparation (diagnostic only), before PresentationResolve.
        DlssInputPreparation,
        // UPSCALING.3C NGX DLSS Super Resolution evaluation, after PresentationResolve.
        DlssEvaluate,
        // UPSCALING.3C.4 Editor overlay depth/mask prepass (jittered Editor frames only), before ToneMap.
        EditorOverlayPrepass,
        // NRD.3 path-traced NRD preparation and bypass reconstruction (diagnostic only), after PathTraceSpatial.
        NrdPathTracePrepare,
        NrdPathTraceResolve,
        // NRD.4B path-traced REBLUR dispatch, between NrdPathTracePrepare and NrdPathTraceResolve.
        NrdPathTraceDenoise,
        // NRD.4B: the Engine denoiser's temporal pass, nested inside PathTraceReference (its spatial pass is
        // PathTraceSpatial).
        PathTraceTemporal,
        // NRD.RELAX.3 diagnostic RELAX dispatch (never presented): preparation, RELAX and the diagnostic resolve, after
        // the path-tracing work of the frame.
        NrdRelaxPrepare,
        NrdRelaxDenoise,
        NrdRelaxResolve,
        NrdHybridPrepare,
        NrdHybridDiffuse,
        NrdHybridSpecular,
        // HYBRID.5: the selected hybrid's production resolve.
        NrdHybridResolve,
        CausticTrace,
        CausticAccumulate,
        CausticComposite,
        CausticClear,
        CausticMaximum,
        CausticInitialize,
        CausticReplay,
        CausticDecode,
        CausticInvalidate,
        CausticFinish,
        CausticReferenceLight,
        CausticReferenceCamera,
        // R.9I.3 pixel-centred caustics: the complete tiled estimator, and the caustics-only debug view.
        PixelCausticEstimate,
        PixelCausticView,
        Count
    };

    struct FrameGap
    {
        uint64_t previousSerial = 0;
        uint64_t currentSerial = 0;

        double milliseconds = 0.0;
    };

    struct Results
    {
        uint64_t frameSerial = 0;
        // Every valid pair resolved in this batch, including older buffered frames.
        FrameGap resolvedGaps[FrameCount]{};
        unsigned int resolvedGapCount = 0;
        double milliseconds[static_cast<unsigned int>(Region::Count)]{};
        uint64_t validRegions = 0;

        bool interFrameGapValid = false;
        bool interFrameGapOutOfOrder = false;

        double interFrameGapMilliseconds = 0.0;
        double interFrameGapAverage = 0.0;
        double interFrameGapMaximum = 0.0;
        unsigned int interFrameGapSampleCount = 0;
    };

    bool Initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue);
    void Shutdown();

    uint64_t GetTimestampFrequency() const;

    void BeginFrame(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex);
    void BeginRegion(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        Region region);
    void EndRegion(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex,
        Region region);
    void EndFrameAndResolve(
        ID3D12GraphicsCommandList* commandList,
        unsigned int frameIndex);

    void MarkSubmitted(
        unsigned int frameIndex,
        uint64_t submissionFence,
        uint64_t frameSerial);

    bool CollectCompleted(uint64_t completedFenceValue);

    const Results& GetResults() const;
    void BreakFrameSequence();

    private:
    static unsigned int GetBeginQueryIndex(
      unsigned int frameIndex,
      Region region);
    static unsigned int GetEndQueryIndex(
        unsigned int frameIndex,
        Region region);

    static constexpr unsigned int RegionCount = static_cast<unsigned int>(Region::Count);
    static_assert(RegionCount <= 64, "GPU region validity masks use 64 bits.");

    static constexpr unsigned int QueriesPerRegion = 2;
    static constexpr unsigned int QueriesPerFrame = RegionCount * QueriesPerRegion;
    static constexpr unsigned int QueryCount = QueriesPerFrame * FrameCount;

    static constexpr uint64_t ReadbackBytesPerFrame = QueriesPerFrame * sizeof(uint64_t);
    static constexpr uint64_t ReadbackBufferSize = ReadbackBytesPerFrame * FrameCount;

    ID3D12QueryHeap* m_queryHeap = nullptr;

    ID3D12Resource* m_readbackBuffer = nullptr;

    uint64_t m_timestampFrequency = 0;

    struct FrameSlot
    {
        uint64_t submissionFence = 0;
        uint64_t frameSerial = 0;

        uint64_t recordedRegions = 0;

        bool pending = false;

        uint64_t sequence = 0;
    };

    FrameSlot m_frameSlots[FrameCount]{};
    Results m_results{};

    void CollectInterFrameGap(
        const FrameSlot& slot,
        const uint64_t* timestamps,
        Results& results);
    uint64_t m_frameSequence = 0;
    uint64_t m_previousBoundarySequence = 0;
    uint64_t m_previousBoundarySerial = 0;
    uint64_t m_previousFrameEnd = 0;

    bool m_previousBoundaryValid = false;
    double m_gapSamples[120]{};
    unsigned int m_gapSampleCount = 0;
    unsigned int m_nextGapSample = 0;
};
