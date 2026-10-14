#pragma once

#include "Core/Log.h"
#include "assets/ProjectMeshCPUCache.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

// Main-thread file-switch diagnostics only. Nested stages are reported separately,
// never added together to manufacture a total. No resource ownership is retained.
namespace SceneSwitchTiming
{
    enum class Stage { Staging, DrainRelease, Renderer, Publication, MeshValidation,
        MeshImport, MeshUpload, TexturePreparation, TextureUpload, AccelerationStructures, ASWait, Count };
    struct Sample
    {
        double milliseconds = 0.0;
        size_t calls = 0;
    };
    struct Statistics
    {
        std::array<Sample, static_cast<size_t>(Stage::Count)> stages{};
        size_t meshInstances = 0;
        size_t uniqueReferences = 0;
        size_t meshCacheHits = 0;
        size_t meshReferenceReuses = 0;
        size_t cpuMeshCacheHits = 0;
        size_t cpuMeshCacheMisses = 0;

        size_t cpuMeshCacheRetainedBytes = 0;
        size_t cpuMeshCachePinnedBytes = 0;
        size_t cpuMeshCacheEvictions = 0;
        size_t cpuMeshCacheBypasses = 0;

        size_t fullDigests = 0;
        size_t imports = 0;
        size_t textureReferences = 0;
        size_t textureDeduplicated = 0;
    };
    inline thread_local Statistics* active = nullptr;
    using Clock = std::chrono::steady_clock;

    class Measure
    {
    public:
        explicit Measure(Stage stage) : m_statistics(active), m_stage(stage),
            m_start(active ? Clock::now() : Clock::time_point{})
        {
        }

        ~Measure()
        {
            Stop();
        }

        void Stop()
        {
            if(!m_statistics)
            {
                return;
            }

            auto& sample = m_statistics->stages[static_cast<size_t>(m_stage)];
            sample.milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
            ++sample.calls;
            m_statistics = nullptr;
        }
        Measure(const Measure&) = delete;
        Measure& operator=(const Measure&) = delete;

    private:
        Statistics* m_statistics;
        Stage m_stage;
        Clock::time_point m_start;
    };

    template<class Function>
    auto MeasureCall(Stage stage, Function&& function)
    {
        Measure measure(stage);
        return function();
    }

    class Session
    {
    public:
        explicit Session(const ProjectMeshCPUCache& cache) : m_cache(cache), m_previous(active), m_start(Clock::now())
        {
            active = &m_statistics;
        }

        ~Session()
        {
            const auto cache = m_cache.GetStatistics();
            m_statistics.cpuMeshCacheRetainedBytes = cache.retainedBytes;
            m_statistics.cpuMeshCachePinnedBytes = cache.pinnedOutsideCacheBytes;
            m_statistics.cpuMeshCacheEvictions = cache.evictions;
            m_statistics.cpuMeshCacheBypasses = cache.bypasses;

            active = m_previous;
            const double total = std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
            Log::Info("File scene switch CPU wall-clock ms: outcome=" + outcome + ", total=" + std::to_string(total) +
                "; outer stages: staging=" + Value(Stage::Staging) + ", drain/release=" + Value(Stage::DrainRelease) +
                ", renderer=" + Value(Stage::Renderer) + ", publication/old cleanup=" + Value(Stage::Publication));
            Log::Info("File scene switch detail CPU ms (overlap outer stages/each other; do not sum): mesh validation/hash=" +
                Value(Stage::MeshValidation) + ", mesh import=" + Value(Stage::MeshImport) +
                ", geometry upload=" + Value(Stage::MeshUpload) + ", texture preparation=" + Value(Stage::TexturePreparation) +
                ", texture upload=" + Value(Stage::TextureUpload) + ", AS initialization=" + Value(Stage::AccelerationStructures) +
                ", AS completion wait=" + Value(Stage::ASWait) + "; GPU execution time=unmeasured");
            Log::Info("File scene switch counts: mesh instances=" + std::to_string(m_statistics.meshInstances) +
                ", unique references resolved=" + std::to_string(m_statistics.uniqueReferences) +
                ", preparation-local reference reuse=" + std::to_string(m_statistics.meshReferenceReuses) +
                ", scene GPU mesh cache hits=" + std::to_string(m_statistics.meshCacheHits) +
                ", completed full source digests=" + std::to_string(m_statistics.fullDigests) +
                ", completed unique imports=" + std::to_string(m_statistics.imports) +
                ", deferred texture references=" + std::to_string(m_statistics.textureReferences) +
                ", shared texture references=" + std::to_string(m_statistics.textureDeduplicated));
            Log::Info("File scene cross-scene CPU mesh cache: hits=" + std::to_string(m_statistics.cpuMeshCacheHits) +
                ", misses=" + std::to_string(m_statistics.cpuMeshCacheMisses) +
                ", retained bytes=" + std::to_string(m_statistics.cpuMeshCacheRetainedBytes) +
                ", consumer-pinned bytes outside cache=" + std::to_string(m_statistics.cpuMeshCachePinnedBytes) +
                ", lifetime evictions=" + std::to_string(m_statistics.cpuMeshCacheEvictions) +
                ", lifetime oversized bypasses=" + std::to_string(m_statistics.cpuMeshCacheBypasses));
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        std::string outcome = "incomplete/failed";

    private:
        std::string Value(Stage stage) const
        {
            const auto& sample = m_statistics.stages[static_cast<size_t>(stage)];
            return sample.calls ? std::to_string(sample.milliseconds) : "n/a (not reached)";
        }

        const ProjectMeshCPUCache& m_cache;

        Statistics m_statistics;
        Statistics* m_previous;
        Clock::time_point m_start;
    };
}
