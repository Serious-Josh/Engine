#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// Diagnostics only: mesh thumbnail work counters, reported with the frame-hitch diagnostics
// (ENGINE_FRAME_HITCH_LOGGING=1) and read by the thumbnail scheduling verification. Counting and
// timing compile to nothing unless ENGINE_HAS_VERIFICATION is defined (Debug diagnostics builds).
namespace MeshThumbnailDiagnostics
{
    struct Counters
    {
        // Asset Browser, main thread.
        std::atomic<std::uint64_t> visibleLookups{0};
        std::atomic<std::uint64_t> browserResolves{0};
        std::atomic<std::uint64_t> browserResolveMicroseconds{0};
        std::atomic<std::uint64_t> browserRequests{0};
        std::atomic<std::uint64_t> browserRequestMicroseconds{0};
        std::atomic<std::uint64_t> admissionReleases{0};
        std::atomic<std::uint64_t> browserDiscards{0};

        // Renderer poll, main thread.
        std::atomic<std::uint64_t> polls{0};
        std::atomic<std::uint64_t> pollMicroseconds{0};

        // Preparation service.
        std::atomic<std::uint64_t> newPreparations{0};
        std::atomic<std::uint64_t> sourceRevalidations{0};
        std::atomic<std::uint64_t> workerDigests{0};
        std::atomic<std::uint64_t> workerDigestMicroseconds{0};
        std::atomic<std::uint64_t> diskCacheHits{0};
        std::atomic<std::uint64_t> fullImports{0};

        // GPU images.
        std::atomic<std::uint64_t> gpuJobsCreated{0};
        std::atomic<std::uint64_t> gpuJobsEvicted{0};
    };

    inline Counters& Get()
    {
        static Counters counters;
        return counters;
    }

#if defined(ENGINE_HAS_VERIFICATION)
    inline constexpr bool kEnabled = true;
#else
    inline constexpr bool kEnabled = false;
#endif

    inline void Add(std::atomic<std::uint64_t>& counter, std::uint64_t value = 1)
    {
        if constexpr(kEnabled)
        {
            counter.fetch_add(value, std::memory_order_relaxed);
        }
    }

    // Adds elapsed wall-clock microseconds to a counter when it leaves scope.
    class Timer
    {
    public:
        explicit Timer(std::atomic<std::uint64_t>& total) : m_total(total)
        {
            if constexpr(kEnabled)
            {
                m_start = std::chrono::steady_clock::now();
            }
        }

        ~Timer()
        {
            if constexpr(kEnabled)
            {
                const auto elapsed = std::chrono::steady_clock::now() - m_start;
                Add(m_total, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
            }
        }

        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

    private:
        std::atomic<std::uint64_t>& m_total;
        std::chrono::steady_clock::time_point m_start;
    };

    // Snapshot of all counters, used to report deltas between two points in time.
    struct Snapshot
    {
        std::uint64_t visibleLookups = 0;
        std::uint64_t browserResolves = 0;
        std::uint64_t browserResolveMicroseconds = 0;
        std::uint64_t browserRequests = 0;
        std::uint64_t browserRequestMicroseconds = 0;
        std::uint64_t admissionReleases = 0;
        std::uint64_t browserDiscards = 0;
        std::uint64_t polls = 0;
        std::uint64_t pollMicroseconds = 0;
        std::uint64_t newPreparations = 0;
        std::uint64_t sourceRevalidations = 0;
        std::uint64_t workerDigests = 0;
        std::uint64_t workerDigestMicroseconds = 0;
        std::uint64_t diskCacheHits = 0;
        std::uint64_t fullImports = 0;
        std::uint64_t gpuJobsCreated = 0;
        std::uint64_t gpuJobsEvicted = 0;
    };

    inline Snapshot Capture()
    {
        const auto& counters = Get();
        const auto read = [](const std::atomic<std::uint64_t>& value)
        {
            return value.load(std::memory_order_relaxed);
        };

        Snapshot snapshot;
        snapshot.visibleLookups = read(counters.visibleLookups);
        snapshot.browserResolves = read(counters.browserResolves);
        snapshot.browserResolveMicroseconds = read(counters.browserResolveMicroseconds);
        snapshot.browserRequests = read(counters.browserRequests);
        snapshot.browserRequestMicroseconds = read(counters.browserRequestMicroseconds);
        snapshot.admissionReleases = read(counters.admissionReleases);
        snapshot.browserDiscards = read(counters.browserDiscards);
        snapshot.polls = read(counters.polls);
        snapshot.pollMicroseconds = read(counters.pollMicroseconds);
        snapshot.newPreparations = read(counters.newPreparations);
        snapshot.sourceRevalidations = read(counters.sourceRevalidations);
        snapshot.workerDigests = read(counters.workerDigests);
        snapshot.workerDigestMicroseconds = read(counters.workerDigestMicroseconds);
        snapshot.diskCacheHits = read(counters.diskCacheHits);
        snapshot.fullImports = read(counters.fullImports);
        snapshot.gpuJobsCreated = read(counters.gpuJobsCreated);
        snapshot.gpuJobsEvicted = read(counters.gpuJobsEvicted);
        return snapshot;
    }

    inline std::string Describe(const Snapshot& from, const Snapshot& to)
    {
        const auto delta = [](std::uint64_t before, std::uint64_t after)
        {
            return std::to_string(after - before);
        };
        const auto milliseconds = [](std::uint64_t before, std::uint64_t after)
        {
            return std::to_string(static_cast<double>(after - before) / 1000.0);
        };

        return "visibleLookups=" + delta(from.visibleLookups, to.visibleLookups) +
            ", browserResolves=" + delta(from.browserResolves, to.browserResolves) +
            " (" + milliseconds(from.browserResolveMicroseconds, to.browserResolveMicroseconds) + " ms)" +
            ", browserRequests=" + delta(from.browserRequests, to.browserRequests) +
            " (" + milliseconds(from.browserRequestMicroseconds, to.browserRequestMicroseconds) + " ms)" +
            ", admissionReleases=" + delta(from.admissionReleases, to.admissionReleases) +
            ", browserDiscards=" + delta(from.browserDiscards, to.browserDiscards) +
            ", polls=" + delta(from.polls, to.polls) +
            " (" + milliseconds(from.pollMicroseconds, to.pollMicroseconds) + " ms)" +
            ", newPreparations=" + delta(from.newPreparations, to.newPreparations) +
            ", sourceRevalidations=" + delta(from.sourceRevalidations, to.sourceRevalidations) +
            ", workerDigests=" + delta(from.workerDigests, to.workerDigests) +
            " (" + milliseconds(from.workerDigestMicroseconds, to.workerDigestMicroseconds) + " ms worker)" +
            ", diskCacheHits=" + delta(from.diskCacheHits, to.diskCacheHits) +
            ", fullImports=" + delta(from.fullImports, to.fullImports) +
            ", gpuJobsCreated=" + delta(from.gpuJobsCreated, to.gpuJobsCreated) +
            ", gpuJobsEvicted=" + delta(from.gpuJobsEvicted, to.gpuJobsEvicted);
    }
}
