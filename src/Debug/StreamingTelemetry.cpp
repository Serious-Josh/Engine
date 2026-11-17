#include "Debug/StreamingTelemetry.h"

#include "Core/Log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace StreamingTelemetry
{
    namespace
    {
        // 16,384 frames is several minutes of streaming at Editor frame rates; older frames are overwritten.
        constexpr std::size_t FrameCapacity = 16384;
        constexpr std::size_t SlowestFrameCount = 12;

        struct FrameRecord
        {
            double executionMilliseconds = 0.0;
            double intervalMilliseconds = 0.0;
            std::array<std::uint64_t, MetricCount> metrics{};
            std::array<std::uint64_t, GaugeCount> gauges{};
        };

        struct State
        {
            std::array<std::atomic<std::uint64_t>, MetricCount> metrics{};
            std::array<std::atomic<std::uint64_t>, GaugeCount> gauges{};

            // Main thread only (EndFrame, BeginWindow, ReportWindow).
            std::vector<FrameRecord> frames;
            std::size_t frameCount = 0;
            std::size_t nextFrame = 0;
            std::array<std::uint64_t, MetricCount> previousFrame{};
            std::array<std::uint64_t, MetricCount> windowStart{};
            Clock::time_point previousFrameStart{};
            Clock::time_point windowStartTime{};
            std::string windowLabel;
            std::uint64_t windowSerial = 0;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        std::thread::id& MainThread()
        {
            static std::thread::id thread;
            return thread;
        }

        std::array<std::uint64_t, MetricCount> CaptureMetrics()
        {
            std::array<std::uint64_t, MetricCount> values{};
            auto& state = GetState();

            for(std::size_t index = 0; index < MetricCount; ++index)
            {
                values[index] = state.metrics[index].load(std::memory_order_relaxed);
            }

            return values;
        }

        bool IsDuration(std::size_t metric)
        {
            const std::string_view name = MetricName(static_cast<Metric>(metric));
            return name.ends_with("Ns");
        }

        double Percentile(std::vector<double> values, double fraction)
        {
            if(values.empty())
            {
                return 0.0;
            }

            const std::size_t index = (std::min)(values.size() - 1,
                static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5));
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
            return values[index];
        }

        std::string FormatMetric(std::size_t metric, std::uint64_t value)
        {
            char text[96];

            if(IsDuration(metric))
            {
                std::snprintf(text, sizeof(text), "%s=%.3fms", MetricName(static_cast<Metric>(metric)), value / 1.0e6);
            }
            else
            {
                std::snprintf(text, sizeof(text), "%s=%llu", MetricName(static_cast<Metric>(metric)),
                    static_cast<unsigned long long>(value));
            }

            return text;
        }

        // The window's frames, oldest first.
        std::vector<const FrameRecord*> WindowFrames()
        {
            auto& state = GetState();
            std::vector<const FrameRecord*> frames;
            frames.reserve(state.frameCount);
            const std::size_t first = (state.nextFrame + FrameCapacity - state.frameCount) % FrameCapacity;

            for(std::size_t offset = 0; offset < state.frameCount; ++offset)
            {
                frames.push_back(&state.frames[(first + offset) % FrameCapacity]);
            }

            return frames;
        }

        void WriteCsv(std::string_view label, const std::vector<const FrameRecord*>& frames)
        {
            char directory[MAX_PATH]{};

            if(!GetEnvironmentVariableA("ENGINE_STREAMING_TELEMETRY_CSV", directory, MAX_PATH))
            {
                return;
            }

            auto& state = GetState();
            std::string fileName = "s0-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(state.windowSerial) + "-";

            for(const char character : label)
            {
                const bool plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' || character == '_';
                fileName += plain ? character : '_';
            }

            const auto path = std::filesystem::path(directory) / (fileName + ".csv");
            std::ofstream file(path);

            if(!file)
            {
                Log::Warning("[S0] cannot write " + path.string());
                return;
            }

            file << "frame,executionMs,intervalMs";

            for(std::size_t metric = 0; metric < MetricCount; ++metric)
            {
                file << ',' << MetricName(static_cast<Metric>(metric));
            }

            for(std::size_t gauge = 0; gauge < GaugeCount; ++gauge)
            {
                file << ',' << GaugeName(static_cast<Gauge>(gauge));
            }

            file << '\n';

            for(std::size_t index = 0; index < frames.size(); ++index)
            {
                const auto& frame = *frames[index];
                file << index << ',' << frame.executionMilliseconds << ',' << frame.intervalMilliseconds;

                for(const auto value : frame.metrics)
                {
                    file << ',' << value;
                }

                for(const auto value : frame.gauges)
                {
                    file << ',' << value;
                }

                file << '\n';
            }

            Log::Info("[S0] frames written to " + path.string());
        }
    }

    bool Enabled()
    {
        static const bool enabled = GetEnvironmentVariableA("ENGINE_STREAMING_TELEMETRY", nullptr, 0) != 0;
        return enabled;
    }

    void SetMainThread()
    {
        MainThread() = std::this_thread::get_id();
    }

    bool IsMainThread()
    {
        return std::this_thread::get_id() == MainThread();
    }

    std::atomic<std::uint64_t>& MetricStorage(Metric metric)
    {
        return GetState().metrics[static_cast<std::size_t>(metric)];
    }

    std::atomic<std::uint64_t>& GaugeStorage(Gauge gauge)
    {
        return GetState().gauges[static_cast<std::size_t>(gauge)];
    }

    std::uint64_t Read(Metric metric)
    {
        return MetricStorage(metric).load(std::memory_order_relaxed);
    }

    void EndFrame(Clock::time_point frameStart)
    {
        if(!Enabled())
        {
            return;
        }

        auto& state = GetState();

        if(state.frames.empty())
        {
            state.frames.resize(FrameCapacity); // One allocation for the process.
        }

        const auto now = CaptureMetrics();
        auto& record = state.frames[state.nextFrame];
        record.executionMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        record.intervalMilliseconds = state.previousFrameStart == Clock::time_point{} ? 0.0 :
            std::chrono::duration<double, std::milli>(frameStart - state.previousFrameStart).count();
        state.previousFrameStart = frameStart;

        for(std::size_t metric = 0; metric < MetricCount; ++metric)
        {
            record.metrics[metric] = now[metric] - state.previousFrame[metric];
        }

        // Queue depths are this frame's maxima; GPU gauges are the GPU results that completed during this frame (0 if none).
        for(std::size_t gauge = 0; gauge < GaugeCount; ++gauge)
        {
            record.gauges[gauge] = state.gauges[gauge].exchange(0, std::memory_order_relaxed);
        }

        state.previousFrame = now;
        state.nextFrame = (state.nextFrame + 1) % FrameCapacity;
        state.frameCount = (std::min)(state.frameCount + 1, FrameCapacity);
    }

    void BeginWindow(std::string_view label)
    {
        if(!Enabled())
        {
            return;
        }

        auto& state = GetState();
        state.frameCount = 0;
        state.windowStart = CaptureMetrics();
        state.windowStartTime = Clock::now();
        state.windowLabel = label;
        ++state.windowSerial;
        Log::Info("[S0] window begin: " + std::string(label));
    }

    void ReportWindow(std::string_view label)
    {
        if(!Enabled())
        {
            return;
        }

        auto& state = GetState();
        const auto now = CaptureMetrics();
        const auto frames = WindowFrames();
        const double seconds = std::chrono::duration<double>(Clock::now() - state.windowStartTime).count();

        // Totals.
        std::string totals;

        for(std::size_t metric = 0; metric < MetricCount; ++metric)
        {
            const std::uint64_t value = now[metric] - state.windowStart[metric];

            if(value != 0)
            {
                totals += " " + FormatMetric(metric, value);
            }
        }

        // Frame-time distribution (execution of Engine::Frame, unsmoothed).
        std::vector<double> execution;
        execution.reserve(frames.size());
        std::size_t over16 = 0;
        std::size_t over33 = 0;
        std::size_t over100 = 0;

        for(const auto* frame : frames)
        {
            execution.push_back(frame->executionMilliseconds);
            over16 += frame->executionMilliseconds > 16.7 ? 1 : 0;
            over33 += frame->executionMilliseconds > 33.3 ? 1 : 0;
            over100 += frame->executionMilliseconds > 100.0 ? 1 : 0;
        }

        const double maximum = execution.empty() ? 0.0 : *std::max_element(execution.begin(), execution.end());
        char summary[512];
        std::snprintf(summary, sizeof(summary),
            "[S0] window %s (%s): %.3f s, %zu frames; frame ms p50=%.2f p95=%.2f p99=%.2f max=%.2f; >16.7ms=%zu >33.3ms=%zu "
            ">100ms=%zu",
            std::string(label).c_str(), state.windowLabel.c_str(), seconds, frames.size(), Percentile(execution, 0.50),
            Percentile(execution, 0.95), Percentile(execution, 0.99), maximum, over16, over33, over100);
        Log::Info(summary);
        Log::Info("[S0] totals" + totals);

        // The slowest frames and what they did.
        std::vector<std::size_t> order(frames.size());

        for(std::size_t index = 0; index < order.size(); ++index)
        {
            order[index] = index;
        }

        const std::size_t slowest = (std::min)(SlowestFrameCount, order.size());
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(slowest), order.end(),
            [&](std::size_t left, std::size_t right)
            {
                return frames[left]->executionMilliseconds > frames[right]->executionMilliseconds;
            });

        for(std::size_t rank = 0; rank < slowest; ++rank)
        {
            const auto& frame = *frames[order[rank]];
            char heading[128];
            std::snprintf(heading, sizeof(heading), "[S0]   slow #%zu frame %zu: %.2f ms (interval %.2f):", rank + 1, order[rank],
                frame.executionMilliseconds, frame.intervalMilliseconds);
            std::string line = heading;

            for(std::size_t metric = 0; metric < MetricCount; ++metric)
            {
                if(frame.metrics[metric] != 0)
                {
                    line += " " + FormatMetric(metric, frame.metrics[metric]);
                }
            }

            for(std::size_t gauge = 0; gauge < GaugeCount; ++gauge)
            {
                if(frame.gauges[gauge] != 0)
                {
                    line += " " + std::string(GaugeName(static_cast<Gauge>(gauge))) + "=" + std::to_string(frame.gauges[gauge]);
                }
            }

            Log::Info(line);
        }

        // Gauge maxima over the window.
        std::string gauges;

        for(std::size_t gauge = 0; gauge < GaugeCount; ++gauge)
        {
            std::uint64_t windowMaximum = 0;

            for(const auto* frame : frames)
            {
                windowMaximum = (std::max)(windowMaximum, frame->gauges[gauge]);
            }

            gauges += " " + std::string(GaugeName(static_cast<Gauge>(gauge))) + "Max=" + std::to_string(windowMaximum);
        }

        Log::Info("[S0] gauges" + gauges);
        WriteCsv(label, frames);
    }

    const char* MetricName(Metric metric)
    {
        static constexpr std::array<const char*, MetricCount> names = {
            "UploadCalls", "UploadCallsOffMainThread", "UploadBytes", "UploadStagingCreateNs", "UploadCopyNs", "UploadWaitNs",
            "UploadTotalNs",
            "FrameFenceWaitCalls", "FrameFenceWaitNs", "GpuIdleWaitCalls", "GpuIdleWaitNs",
            "BlasBuilds", "BlasAllocations", "BlasResultBytes", "BlasScratchBytes", "BlasRecordNs", "BlasRetirementReleases",
            "TlasBuilds", "TlasBuildInstances", "TlasBuildBytes", "TlasBuildNs", "TlasUpdates", "TlasUpdateInstances",
            "TlasUpdateNs",
            "CandidatePrepares", "CandidateProposals", "CandidatePrepareNs", "CandidateSlotWaitNs", "CandidateTransformRefreshes",
            "PublicationAttempts", "Publications", "PublicationEntries", "PublicationNs",
            "ResidencyBatchesSubmitted", "ResidencyBatchEntries", "ResidencySingleRequests", "ResidencyAdvanceNs",
            "TextureStreamingNs",
            "PublicationIdentityNs", "PublicationPollNs", "PublicationSourceValidationNs", "PublicationAdoptionNs",
            "PublicationHandoffNs", "PublicationApplyNs", "PublicationCommitNs", "CandidateBuildNs", "CandidateRecordNs",
            "MeshJobs", "MeshJobQueueNs", "MeshJobWorkNs", "TextureJobs", "TextureJobQueueNs", "TextureJobWorkNs",
            "MaterialJobs", "MaterialJobQueueNs", "MaterialJobWorkNs",
            "SceneSwitches", "SceneSwitchNs", "SceneDrainReleaseNs", "SceneInitialAsNs", "SceneInitialAsWaitNs",
            "SceneActivatedCallbackNs", "SceneEditorActivatedNs", "SceneEnvironmentNs",
            "ObjectMetadataCalls", "ObjectMetadataNs", "SceneDocumentValidationNs",
            "MeshPrefetchPollNs", "MeshPrefetchIntegrations", "MeshPrefetchCacheNs", "MeshPrefetchResolveNs",
            "MeshPrefetchRequestNs", "ResidencyDescribeCalls", "ResidencyDescribeNs",
            "EnvironmentRequests", "EnvironmentWorkerNs", "EnvironmentCreateNs", "EnvironmentRecordNs", "EnvironmentRecordFrames",
            "EnvironmentCopyBytes", "EnvironmentWorkUnits", "EnvironmentPublishNs", "EnvironmentPublications",
            "EnvironmentSuperseded", "EnvironmentRetirementReleases", "SceneStagingSteps", "SceneStagingNs",
            "GeometryUploadsQueued", "GeometryUploadBytes", "GeometryUploadRecordNs",
        };

        return names[static_cast<std::size_t>(metric)];
    }

    const char* GaugeName(Gauge gauge)
    {
        static constexpr std::array<const char*, GaugeCount> names = {
            "MeshQueueDepth", "TextureQueueDepth", "MaterialQueueDepth", "GpuFrameUs", "GpuTlasUpdateUs",
        };

        return names[static_cast<std::size_t>(gauge)];
    }
}
