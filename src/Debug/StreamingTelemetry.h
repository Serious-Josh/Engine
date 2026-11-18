#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// S0.0 scene-streaming diagnostics. Observational only: nothing here changes streaming, scheduling, synchronization or
// resource ownership. Off unless the process starts with ENGINE_STREAMING_TELEMETRY set; disabled, every call is one cached
// flag test and nothing is allocated.
//
// Enabled:
// - metrics are process-wide relaxed atomics (counts, bytes, nanoseconds), written by any thread;
// - EndFrame, called once per Engine frame on the main thread, stores each metric's change during that frame in a
//   preallocated ring, with the unsmoothed frame time;
// - a measurement window (BeginWindow .. ReportWindow) logs totals, the frame-time distribution and the slowest frames
//   with what happened in them. ENGINE_STREAMING_TELEMETRY_CSV=<directory> also writes the window's frames as CSV.
//
// Times are CPU wall-clock time on the calling thread. "Wait" metrics are CPU time blocked on a fence or event, so they
// include GPU execution and queue latency; GPU execution alone comes from the GPU timestamp gauges.
namespace StreamingTelemetry
{
    enum class Metric : std::uint32_t
    {
        // ResourceUploader: one call per UploadBuffer/UploadTexture.
        UploadCalls,
        UploadCallsOffMainThread,
        UploadBytes,
        UploadStagingCreateNs,
        UploadCopyNs,
        UploadWaitNs,
        UploadTotalNs,

        // CommandContext waits (CPU blocked on GPU completion).
        FrameFenceWaitCalls,
        FrameFenceWaitNs,
        GpuIdleWaitCalls,
        GpuIdleWaitNs,

        // Bottom-level acceleration structures (every BLAS recording: initial scene, candidate, rebuild).
        BlasBuilds,
        BlasAllocations,
        BlasResultBytes,
        BlasScratchBytes,
        BlasRecordNs,
        BlasRetirementReleases,

        // Top-level acceleration structures.
        TlasBuilds,
        TlasBuildInstances,
        TlasBuildBytes,
        TlasBuildNs,
        TlasUpdates,
        TlasUpdateInstances,
        TlasUpdateNs,

        // Mesh residency: candidate preparation (renderer), adoption (Engine boundary), automatic scheduling.
        CandidatePrepares,
        CandidateProposals,
        CandidatePrepareNs,
        CandidateSlotWaitNs,
        CandidateTransformRefreshes,
        PublicationAttempts,
        Publications,
        PublicationEntries,
        PublicationNs,
        ResidencyBatchesSubmitted,
        ResidencyBatchEntries,
        ResidencySingleRequests,
        ResidencyAdvanceNs,
        TextureStreamingNs,

        // Publication and candidate detail (nested inside PublicationNs / CandidatePrepareNs; do not add to them).
        PublicationIdentityNs,
        PublicationPollNs,
        PublicationSourceValidationNs,
        PublicationAdoptionNs,
        PublicationApplyNs,
        CandidateBuildNs,
        CandidateRecordNs,

        // Preparation workers: jobs finished, time queued before the worker took them, and time processing.
        MeshJobs,
        MeshJobQueueNs,
        MeshJobWorkNs,
        TextureJobs,
        TextureJobQueueNs,
        TextureJobWorkNs,
        MaterialJobs,
        MaterialJobQueueNs,
        MaterialJobWorkNs,

        // Scene switch (Engine main thread).
        SceneSwitches,
        SceneSwitchNs,
        SceneDrainReleaseNs,
        SceneInitialAsNs,
        SceneInitialAsWaitNs,
        SceneActivatedCallbackNs,
        SceneEditorActivatedNs,
        SceneEnvironmentNs,

        // FileSceneRuntime metadata lookups: each validates the whole scene state (document + mapping).
        ObjectMetadataCalls,
        ObjectMetadataNs,
        SceneDocumentValidationNs,

        // S0.4 import prefetch (inside ResidencyAdvanceNs): service polling, and catalog integration split into cache
        // integration and source-validated GPU geometry preparation.
        MeshPrefetchPollNs,
        MeshPrefetchIntegrations,
        MeshPrefetchCacheNs,
        MeshPrefetchResolveNs,
        MeshPrefetchRequestNs,
        ResidencyDescribeCalls,
        ResidencyDescribeNs,

        // S0.5 asynchronous scene environment: requests, worker time (decode, calibration, importance CDF, staging writes),
        // main-thread resource creation, per-frame command recording (copies and precompute), the GPU work it issued, and
        // publication. Superseded counts requests replaced before publishing; releases count fence-retired owners.
        EnvironmentRequests,
        EnvironmentWorkerNs,
        EnvironmentCreateNs,
        EnvironmentRecordNs,
        EnvironmentRecordFrames,
        EnvironmentCopyBytes,
        EnvironmentWorkUnits,
        EnvironmentPublishNs,
        EnvironmentPublications,
        EnvironmentSuperseded,
        EnvironmentRetirementReleases,

        // S0.7B staged activation: frames spent staging a requested file scene before its switch, and their time.
        SceneStagingSteps,
        SceneStagingNs,

        // S0.8B asynchronous geometry uploads: batches queued, bytes recorded into frames, and the recording time.
        GeometryUploadsQueued,
        GeometryUploadBytes,
        GeometryUploadRecordNs,

        // W6.0 scene documents: files presented to SceneDocumentCache after their consumer read them, cache hits and the
        // exact-content comparison each costs, document copies made for consumers that edit, and every full scene parse
        // (through the cache or not; a write's read-back verification is one).
        SceneDocumentReads,
        SceneDocumentReadBytes,
        SceneDocumentCacheHits,
        SceneDocumentCompareNs,
        SceneDocumentCopies,
        SceneDocumentCopyNs,
        SceneParses,
        SceneParseBytes,
        SceneParseNs,

        Count
    };

    // Per-frame maxima and last values: reset by EndFrame, recorded, never summed across frames.
    enum class Gauge : std::uint32_t
    {
        MeshQueueDepth,
        TextureQueueDepth,
        MaterialQueueDepth,
        GpuFrameMicroseconds,
        GpuTlasUpdateMicroseconds,
        Count
    };

    inline constexpr std::size_t MetricCount = static_cast<std::size_t>(Metric::Count);
    inline constexpr std::size_t GaugeCount = static_cast<std::size_t>(Gauge::Count);

    using Clock = std::chrono::steady_clock;

    bool Enabled();

    // Engine startup records its thread, so upload calls from other threads can be counted separately.
    void SetMainThread();
    bool IsMainThread();

    std::atomic<std::uint64_t>& MetricStorage(Metric metric);
    std::atomic<std::uint64_t>& GaugeStorage(Gauge gauge);

    inline void Add(Metric metric, std::uint64_t value)
    {
        if(Enabled())
        {
            MetricStorage(metric).fetch_add(value, std::memory_order_relaxed);
        }
    }

    inline void Raise(Gauge gauge, std::uint64_t value)
    {
        if(!Enabled())
        {
            return;
        }

        auto& storage = GaugeStorage(gauge);
        std::uint64_t current = storage.load(std::memory_order_relaxed);

        while(current < value && !storage.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {
        }
    }

    inline std::uint64_t ElapsedNanoseconds(Clock::time_point start)
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }

    // A preparation worker took a job: time it spent queued (when its queue time was stamped) and the queue depth then.
    inline void RecordJobStart(Clock::time_point queuedAt, Metric queueWait, Gauge queueDepth, std::uint64_t depth)
    {
        if(!Enabled())
        {
            return;
        }

        if(queuedAt != Clock::time_point{})
        {
            Add(queueWait, ElapsedNanoseconds(queuedAt));
        }

        Raise(queueDepth, depth);
    }

    // Stamps a job's enqueue time (only when enabled, so a disabled process never reads the clock).
    inline Clock::time_point QueueStamp()
    {
        return Enabled() ? Clock::now() : Clock::time_point{};
    }

    // Adds the scope's duration to `duration` and, when given, one to `count`.
    class Scope
    {
    public:
        Scope(Metric duration, Metric count)
            : m_enabled(Enabled()), m_duration(duration), m_count(count), m_hasCount(true)
        {
            if(m_enabled)
            {
                m_start = Clock::now();
            }
        }

        explicit Scope(Metric duration)
            : m_enabled(Enabled()), m_duration(duration), m_count(duration), m_hasCount(false)
        {
            if(m_enabled)
            {
                m_start = Clock::now();
            }
        }

        ~Scope()
        {
            if(!m_enabled)
            {
                return;
            }

            MetricStorage(m_duration).fetch_add(ElapsedNanoseconds(m_start), std::memory_order_relaxed);

            if(m_hasCount)
            {
                MetricStorage(m_count).fetch_add(1, std::memory_order_relaxed);
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        bool m_enabled = false;
        Metric m_duration;
        Metric m_count;
        bool m_hasCount = false;
        Clock::time_point m_start{};
    };

    // Main thread, once per Engine frame: `frameStart` is when this frame began.
    void EndFrame(Clock::time_point frameStart);

    // Starts a new window: clears the frame ring and records the metric baseline.
    void BeginWindow(std::string_view label);

    // Logs the window since BeginWindow, without ending it.
    void ReportWindow(std::string_view label);

    // Current cumulative value (for harness checks).
    std::uint64_t Read(Metric metric);

    const char* MetricName(Metric metric);
    const char* GaugeName(Gauge gauge);
}
