#include "Engine/VoxelResidencyController.h"

#include "Core/Log.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <exception>

using Voxel::Residency::Planner;
using Voxel::Residency::TableEntry;

namespace
{
// A read error that is not "not registered" is a file-system failure, which may pass; everything else is permanent.
bool IsTransientReadError(const std::string& message)
{
    return message.rfind("No registered", 0) != 0 && message.find("larger than any voxel asset") == std::string::npos;
}
}

VoxelResidencyController::VoxelResidencyController(Settings settings) :
    m_settings(settings),
    m_planner(settings.planner)
{
    m_thread = std::thread([this] { Run(); });
}

VoxelResidencyController::~VoxelResidencyController()
{
    Stop();
}

void VoxelResidencyController::Stop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_jobs.clear();
    }

    m_wake.notify_all();

    if(m_thread.joinable())
    {
        m_thread.join();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// Worker

void VoxelResidencyController::Run()
{
    for(;;)
    {
        Job job;

        {
            std::unique_lock lock(m_mutex);

            // Bounded: no new work while enough packed regions wait for publication.
            m_wake.wait(lock, [this] { return m_stopping || (!m_jobs.empty() && m_results.size() < m_settings.maximumPendingResults); });

            if(m_stopping)
            {
                return;
            }

            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_busy = true;
        }

        Result result = Process(job);

        {
            std::lock_guard lock(m_mutex);
            m_busy = false;
            (job.manifest ? m_manifestsDecoded : m_decoded) += 1;

            // A reset meanwhile makes the result meaningless.
            if(job.generation == m_generation)
            {
                m_results.push_back(std::move(result));
            }
        }
    }
}

VoxelResidencyController::Result VoxelResidencyController::Process(const Job& job)
{
    Result result;
    result.generation = job.generation;
    result.manifest = job.manifest;
    result.key = job.key;
    result.reference = job.reference;

    try
    {
        const auto bytes = job.store->Read(job.reference);

        if(!bytes)
        {
            result.error = bytes.error();
            result.transient = IsTransientReadError(bytes.error());
            return result;
        }

        if(job.manifest)
        {
            Voxel::ManifestData manifest;

            if(const Voxel::DecodeResult decoded = Voxel::DecodeManifest(*bytes, manifest); !decoded)
            {
                result.error = "The voxel manifest does not decode: " + decoded.message;
                return result;
            }

            result.manifestData = std::move(manifest);
            return result;
        }

        const auto entry = std::find_if(job.listing->entries.begin(), job.listing->entries.end(),
            [&](const Voxel::ManifestEntry& listed) { return listed.key == job.key; });

        if(entry == job.listing->entries.end())
        {
            result.error = "The region is not listed by its manifest.";
            return result;
        }

        Voxel::RegionData data;
        result.error = Voxel::Residency::DecodeListedRegion(*job.listing, *entry, *bytes, data);

        if(!result.error.empty())
        {
            return result;
        }

        auto packed = Voxel::Residency::PackRegion(data);

        if(!packed)
        {
            result.error = packed.error();
            return result;
        }

        result.region = std::move(*packed);
    }
    catch(const std::exception& exception)
    {
        // Allocation failure: worth retrying.
        result.error = std::string("Loading a voxel asset failed: ") + exception.what();
        result.transient = true;
    }

    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Main thread

void VoxelResidencyController::Enqueue(Job job)
{
    {
        std::lock_guard lock(m_mutex);
        job.generation = m_generation;
        m_jobs.push_back(std::move(job));
    }

    m_wake.notify_one();
}

// Requests the planner no longer wants (evicted, or superseded by a new manifest) are dropped before they are decoded.
void VoxelResidencyController::PruneJobs()
{
    std::lock_guard lock(m_mutex);
    const std::size_t before = m_jobs.size();
    std::erase_if(m_jobs, [this](const Job& job) { return !job.manifest && !m_planner.IsWanted(job.key, job.reference); });
    m_statistics.requestsPruned += before - m_jobs.size();
}

void VoxelResidencyController::ResetAll(Renderer& renderer)
{
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_jobs.clear();
        m_results.clear();
    }

    m_wake.notify_all();
    m_planner = Planner(m_settings.planner);
    m_deferred.clear();
    m_keySlots.clear();
    m_slotKeys.clear();
    m_uploading.clear();
    m_reportedFailures.clear();
    m_manifestReference.clear();
    m_store.reset();
    m_manifest.reset();
    m_configured = false;
    m_initialPublished = false;
    m_tableDirty = false;
    m_awaitingManifest = false;
    renderer.ResetVoxelResidency();
}

void VoxelResidencyController::ApplyManifest(const Voxel::ManifestData& manifest, Renderer& renderer)
{
    // The planner's window for this manifest decides the GPU configuration; a different one is a fresh start.
    Planner probe(m_settings.planner);
    probe.SetManifest(manifest);
    const bool sameSpace = m_configured && manifest.regionBricks == m_regionBricks && manifest.baseExponent == m_baseExponent &&
        probe.Window() == m_window;


    if(!sameSpace)
    {
        m_planner = Planner(m_settings.planner);
        m_deferred.clear();
        m_keySlots.clear();
        m_slotKeys.clear();
        m_uploading.clear();
        std::string error;
        m_configured = renderer.ConfigureVoxelResidency(manifest.regionBricks, manifest.baseExponent, probe.Window(), &error);
        m_regionBricks = manifest.regionBricks;
        m_baseExponent = manifest.baseExponent;
        m_window = probe.Window();
        m_initialPublished = false;

        if(!m_configured)
        {
            Log::Error("Voxel residency: " + error);
            return;
        }
    }

    // Unchanged resident regions are kept; Plan reports the others as evictions.
    m_planner.SetManifest(manifest);
    m_manifest = std::make_shared<const Voxel::ManifestData>(manifest);
    m_tableDirty = true;
}

void VoxelResidencyController::TakeResults(double now, Renderer& renderer)
{
    std::deque<Result> results;

    {
        std::lock_guard lock(m_mutex);
        results.swap(m_results);
    }

    m_wake.notify_one();

    // Regions the Renderer could not take last frame (storage still retiring) go first.
    while(!m_deferred.empty())
    {
        results.push_front(std::move(m_deferred.back()));
        m_deferred.pop_back();
    }

    for(Result& result : results)
    {
        if(result.manifest)
        {
            if(result.reference != m_manifestReference)
            {
                ++m_statistics.resultsDropped;
                continue;
            }

            m_awaitingManifest = false;

            if(!result.manifestData)
            {
                // Without a readable manifest nothing is known: everything resident is evicted and every query is unknown.
                Log::Error("Voxel residency: the scene's voxel manifest cannot be used: " + result.error);
                m_planner.ClearManifest();
                m_manifest.reset();
                continue;
            }

            ApplyManifest(*result.manifestData, renderer);
            continue;
        }

        if(!m_planner.IsWanted(result.key, result.reference))
        {
            ++m_statistics.resultsDropped;
            continue;
        }

        if(!result.region)
        {
            ++m_statistics.loadFailures;
            m_planner.OnLoadFailed(result.key, result.transient, now, result.error);
            m_tableDirty = true;

            if(m_reportedFailures.insert(result.key).second)
            {
                Log::Warning("Voxel residency: region (" + std::to_string(result.key.x) + ", " + std::to_string(result.key.y) + ", " +
                    std::to_string(result.key.z) + ") is unavailable: " + result.error);
            }

            continue;
        }

        const auto slot = renderer.AddVoxelResidencyRegion(*result.region);

        if(!slot)
        {
            // Within the planner's ceilings, so storage is only waiting for the GPU to release it: retry next frame.
            m_deferred.push_back(std::move(result));
            continue;
        }

        m_keySlots[result.key] = *slot;
        m_slotKeys[*slot] = result.key;
        m_uploading.insert(*slot);
        ++m_statistics.regionsPublished;
    }

    {
        std::lock_guard lock(m_mutex);
        m_statistics.regionsDecoded = m_decoded;
        m_statistics.manifestsDecoded = m_manifestsDecoded;
    }

    // Finished uploads become resident in the planner, so the next table names them.
    for(const std::uint32_t slot : renderer.TakeUploadedVoxelResidencySlots())
    {
        const auto key = m_slotKeys.find(slot);

        if(key != m_slotKeys.end() && m_uploading.erase(slot) > 0)
        {
            m_planner.OnLoaded(key->second, slot);
            m_tableDirty = true;
        }
    }
}

void VoxelResidencyController::PublishTable(Renderer& renderer)
{
    if(!m_configured || !m_tableDirty || !m_planner.HasManifest())
    {
        return;
    }

    // The first table of a configuration waits until the initial window has loaded or failed.
    if(!m_initialPublished)
    {
        const auto& records = m_planner.Records();
        const bool loading = std::any_of(records.begin(), records.end(),
            [](const auto& record) { return record.second.status == Planner::RegionRecord::Status::Loading; });

        if(loading)
        {
            return;
        }
    }

    renderer.SetVoxelResidencyTable(m_planner.BuildTable());
    m_initialPublished = true;
    m_tableDirty = false;
    ++m_statistics.tablesSet;
}

void VoxelResidencyController::Update(std::uint64_t sceneIdentity, const std::string& manifestReference,
    const std::function<std::shared_ptr<const Voxel::AssetStore>()>& makeStore, const std::array<double, 3>& camera, double now,
    Renderer& renderer)
{
    // A different scene (or none): nothing of the previous one survives.
    if(sceneIdentity != m_scene)
    {
        ResetAll(renderer);
        m_scene = sceneIdentity;
    }

    // A rebake, undo or redo: answer unknown at once, read the new manifest, and keep unchanged regions once it is known.
    if(manifestReference != m_manifestReference)
    {
        m_manifestReference = manifestReference;

        if(m_configured && m_initialPublished)
        {
            renderer.SetVoxelResidencyTable(std::vector<TableEntry>(std::size_t{m_window} * m_window * m_window));
        }

        // Residency is kept, not cleared: the new manifest decides what stays. Planning pauses until it is known.
        m_tableDirty = false;

        if(manifestReference.empty() || sceneIdentity == 0)
        {
            m_planner.ClearManifest();
            m_manifest.reset();
            m_store.reset();
            m_awaitingManifest = false;
        }
        else
        {
            m_awaitingManifest = true;
            m_store = makeStore();
            Job job;
            job.manifest = true;
            job.reference = manifestReference;
            job.store = m_store;
            Enqueue(std::move(job));
        }
    }

    TakeResults(now, renderer);

    if(m_awaitingManifest)
    {
        return;
    }

    if(m_planner.HasManifest() && m_configured)
    {
        m_tableDirty = m_planner.SetCamera(camera) || m_tableDirty;
    }

    // Evictions leave the table now; requests go to the worker, nearest first.
    const Planner::Update update = m_planner.Plan(now);
    m_statistics.overBudget = update.overBudget;

    for(const Voxel::RegionKey& key : update.evict)
    {
        const auto slot = m_keySlots.find(key);

        if(slot != m_keySlots.end())
        {
            renderer.RemoveVoxelResidencyRegion(slot->second);
            m_uploading.erase(slot->second);
            m_slotKeys.erase(slot->second);
            m_keySlots.erase(slot);
            ++m_statistics.regionsEvicted;
        }

        m_tableDirty = true;
    }

    PruneJobs();

    for(const Planner::Request& request : update.load)
    {
        Job job;
        job.key = request.key;
        job.reference = request.reference;
        job.store = m_store;
        job.listing = m_manifest;
        Enqueue(std::move(job));
    }

    PublishTable(renderer);
}

bool VoxelResidencyController::IsSettled() const
{
    std::lock_guard lock(m_mutex);
    const bool loading = std::any_of(m_planner.Records().begin(), m_planner.Records().end(),
        [](const auto& record) { return record.second.status == Planner::RegionRecord::Status::Loading; });
    return m_jobs.empty() && m_results.empty() && !m_busy && m_deferred.empty() && m_uploading.empty() && !loading && !m_tableDirty &&
        !m_awaitingManifest;
}
