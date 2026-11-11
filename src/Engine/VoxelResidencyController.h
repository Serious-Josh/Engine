#pragma once

#include "Voxel/VoxelAssetStore.h"
#include "Voxel/VoxelManifest.h"
#include "Voxel/VoxelResidency.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class Renderer;

// VX.5 Step 4: keeps the active scene's baked voxel regions near the camera resident on the GPU (VoxelArchitecture.md §6.10).
//
// Main thread, once per frame before Render (Engine::Frame): it follows the scene's published manifest and the camera, asks the
// Step 1 Planner what should be resident, hands load requests to one background worker, publishes finished regions through the
// Renderer's residency API, and sets the table once its regions are uploaded. It is independent of the Editor overlay and of
// any consumer.
//
// The worker reads, validates (DecodeListedRegion) and packs one asset at a time through a read-only ProjectAssetStore. Its
// queue is pruned every frame of requests the planner no longer wants, and at most maximumPendingResults packed regions wait
// for publication, so memory stays bounded.
//
// The binding stays invalid until the first table of a configuration, published once the initial window has loaded (or
// failed). When the manifest changes (rebake, undo, redo), an all-unknown table is set at once, so the previous bake is never
// answered for; the new manifest keeps every unchanged resident region. A scene change resets everything.
class VoxelResidencyController
{
public:
    struct Settings
    {
        Voxel::Residency::PlannerSettings planner;
        std::size_t maximumPendingResults = 4;
    };

    explicit VoxelResidencyController(Settings settings = {});
    ~VoxelResidencyController();
    VoxelResidencyController(const VoxelResidencyController&) = delete;
    VoxelResidencyController& operator=(const VoxelResidencyController&) = delete;

    // Main thread, between frames. `sceneIdentity` 0 means no file scene; `manifestReference` empty means no bake. `makeStore`
    // is called only when the manifest changes, to read that manifest's assets.
    void Update(std::uint64_t sceneIdentity, const std::string& manifestReference,
        const std::function<std::shared_ptr<const Voxel::AssetStore>()>& makeStore, const std::array<double, 3>& camera, double now,
        Renderer& renderer);

    // Joins the worker; nothing is published afterwards. Before the Renderer shuts down.
    void Stop() noexcept;

    // Diagnostics.
    struct Statistics
    {
        std::uint64_t regionsDecoded = 0;
        std::uint64_t manifestsDecoded = 0;
        std::uint64_t regionsPublished = 0;
        std::uint64_t regionsEvicted = 0;
        std::uint64_t resultsDropped = 0;
        std::uint64_t requestsPruned = 0;
        std::uint64_t loadFailures = 0;
        std::uint64_t tablesSet = 0;
        std::size_t overBudget = 0;
    };

    const Statistics& GetStatistics() const noexcept
    {
        return m_statistics;
    }

    // True when nothing is queued, being decoded, waiting for publication or uploading.
    bool IsSettled() const;

private:
    friend class VoxelStreamingVerification;

    struct Job
    {
        std::uint64_t generation = 0;
        bool manifest = false;
        Voxel::RegionKey key;
        std::string reference;
        std::shared_ptr<const Voxel::AssetStore> store;
        std::shared_ptr<const Voxel::ManifestData> listing;
    };

    struct Result
    {
        std::uint64_t generation = 0;
        bool manifest = false;
        Voxel::RegionKey key;
        std::string reference;
        std::optional<Voxel::ManifestData> manifestData;
        std::optional<Voxel::Residency::PackedRegion> region;
        std::string error;
        bool transient = false;
    };

    void Run();
    static Result Process(const Job& job);
    void ResetAll(Renderer& renderer);
    void ApplyManifest(const Voxel::ManifestData& manifest, Renderer& renderer);
    void Enqueue(Job job);
    void PruneJobs();
    void TakeResults(double now, Renderer& renderer);
    void PublishTable(Renderer& renderer);

    Settings m_settings;
    Voxel::Residency::Planner m_planner;

    // Main-thread state.
    std::uint64_t m_scene = 0;
    std::string m_manifestReference;
    std::shared_ptr<const Voxel::AssetStore> m_store;
    std::shared_ptr<const Voxel::ManifestData> m_manifest;
    bool m_configured = false;
    std::uint32_t m_regionBricks = 0;
    int m_baseExponent = 0;
    std::uint32_t m_window = 0;
    bool m_initialPublished = false;
    bool m_tableDirty = false;
    // The manifest reference changed and its manifest is being read: residency is kept but nothing is planned.
    bool m_awaitingManifest = false;
    std::map<Voxel::RegionKey, std::uint32_t> m_keySlots;
    std::map<std::uint32_t, Voxel::RegionKey> m_slotKeys;
    std::set<std::uint32_t> m_uploading;
    std::deque<Result> m_deferred;
    std::set<Voxel::RegionKey> m_reportedFailures;
    Statistics m_statistics;

    // Shared with the worker.
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_jobs;
    std::deque<Result> m_results;
    bool m_busy = false;
    bool m_stopping = false;
    std::uint64_t m_generation = 1;
    std::uint64_t m_decoded = 0;
    std::uint64_t m_manifestsDecoded = 0;
    std::thread m_thread;
};
