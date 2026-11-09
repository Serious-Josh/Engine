#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"
#include "Editor/ProbeSceneAccess.h"
#include "Engine/ReflectionBakeJobs.h"
#include "Engine/ReflectionBakeStatus.h"
#include "assets/AcousticMaterial.h"
#include "assets/AssetID.h"
#include "assets/ProjectAssetResolver.h"

#include <imgui.h>

#include <algorithm>
#include <limits>

// M8.3: the Editor's side of reflection baking. A run is a queue of Probe Volumes, baked one at a time on the audio system's
// acoustic job thread. Each volume goes through the same steps whether the run is Bake All or one volume:
//
//   evaluate afresh -> (generate probes first, through the ordinary generation, when they are missing or stale) -> bake ->
//   assemble and verify the asset -> wait until the scene accepts edits -> evaluate afresh again -> store -> commit the reference.
//
// Nothing is published unless the last evaluation still matches what was baked, so an edit, undo or redo made during the bake
// discards its result. The previous asset and reference are never touched by a failed, cancelled or discarded bake.
namespace
{
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);
    const ImVec4 ValidColor(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 ErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
    constexpr auto StatusRefreshInterval = std::chrono::milliseconds(250);
    constexpr auto StatusRecollectInterval = std::chrono::seconds(5);
    constexpr std::size_t ShownLayerReasons = 6;

    std::string DisplayName(const SceneObjectDocument& object)
    {
        return object.name.empty() ? object.id : object.name;
    }

    const SceneObjectDocument* FindObject(const SceneDocument& document, std::string_view id)
    {
        const auto found = std::find_if(document.objects.begin(), document.objects.end(), [id](const SceneObjectDocument& object)
        {
            return object.id == id;
        });
        return found == document.objects.end() ? nullptr : &*found;
    }

    std::string LayerName(const ReflectionBakes::RequiredLayer& layer, const SceneDocument& document)
    {
        if(layer.kind == BakedReflectionsAssets::LayerKind::Reverb)
        {
            return "Listener reverb";
        }

        const SceneObjectDocument* source = FindObject(document, layer.sourceObjectId);
        return "Static source: " + (source ? DisplayName(*source) : layer.sourceObjectId);
    }

    const char* ToString(ReflectionBakes::DisplayState state) noexcept
    {
        switch(state)
        {
        case ReflectionBakes::DisplayState::NotRequired:
            return "Not required (disabled)";
        case ReflectionBakes::DisplayState::Valid:
            return "Valid";
        case ReflectionBakes::DisplayState::Partial:
            return "Partial";
        case ReflectionBakes::DisplayState::Stale:
            return "Stale";
        case ReflectionBakes::DisplayState::Missing:
            return "Not baked";
        case ReflectionBakes::DisplayState::Baking:
            return "Baking";
        case ReflectionBakes::DisplayState::Failed:
            return "Last bake failed";
        }

        return "Unknown";
    }

    std::optional<ReflectionBakeStatus::FileStamp> StampOf(const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string_view reference)
    {
        const auto id = AssetID::ParseReference(reference);

        if(!id)
        {
            return std::nullopt;
        }

        const AssetResolution resolution = ProjectAssetResolver::Resolve(paths, registry, *id);

        if(resolution.status != AssetAvailability::Available)
        {
            return std::nullopt;
        }

        std::error_code error;
        ReflectionBakeStatus::FileStamp stamp;
        stamp.time = std::filesystem::last_write_time(resolution.resolvedPath, error);

        if(error)
        {
            return std::nullopt;
        }

        stamp.size = std::filesystem::file_size(resolution.resolvedPath, error);
        return error ? std::nullopt : std::optional(stamp);
    }
}

ReflectionBakeStatus::Host Editor::MakeReflectionBakeHost(const FileBackedDemoScene& owner, const IDemoScene& scene, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry) const
{
    ReflectionBakeStatus::Host host;
    host.access = ProbeSceneAccess::MakeSceneAccess(*owner.m_runtime, scene);
    host.resolveMaterial = [&paths, &registry](const std::string& reference, std::string* error)
    {
        return AcousticMaterials::Resolve(reference, &paths, &registry, error);
    };
    host.stamp = [&paths, &registry](std::string_view reference)
    {
        return StampOf(paths, registry, reference);
    };
    host.loadProbes = [&paths, &registry](std::string_view reference)
    {
        return ProbeBatchAssets::Resolve(reference, paths, registry);
    };
    host.loadBaked = [&paths, &registry](std::string_view reference)
    {
        return BakedReflectionsAssets::Resolve(reference, paths, registry);
    };
    // The revisions the ray tracing scene uses to notice mesh changes: structure (objects and their meshes) and residency.
    host.geometryRevision = [&owner]
    {
        return owner.m_runtime->GetScene().GetStructureRevision() * 0x9e3779b97f4a7c15ull ^ owner.m_runtime->GetResidencyRevision();
    };
    return host;
}

void Editor::PollAcousticJobs(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    PollProbeGeneration(scene);

    if(const auto force = std::exchange(m_bakeAllRequest, std::nullopt))
    {
        StartBakeAll(scene, *force);
    }

    PollReflectionBake(scene, paths, registry);
    PollPathingBake(scene, paths, registry);
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument())
    {
        m_reflectionBakeStatus.Invalidate();
        return;
    }

    // Display only, so throttled; bakes and publication evaluate afresh.
    const auto now = std::chrono::steady_clock::now();

    if(m_gameState != GameState::Editing || now < m_reflectionBakeStatusNext)
    {
        return;
    }

    m_reflectionBakeStatusNext = now + StatusRefreshInterval;
    const bool recollect = now >= m_reflectionBakeRecollectNext;

    if(recollect)
    {
        m_reflectionBakeRecollectNext = now + StatusRecollectInterval;
    }

    m_reflectionBakeStatus.Refresh(owner->m_runtime->GetFileScene().GetDocument(), MakeReflectionBakeHost(*owner, scene, paths, registry),
        recollect);
}

std::optional<std::string> Editor::AcousticGeometryNotReady(const IDemoScene& scene) const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner || !m_acousticGeometryReadiness)
    {
        return std::nullopt;
    }

    return m_acousticGeometryReadiness(*owner);
}

void Editor::StartReflectionBake(IDemoScene& scene, std::vector<std::string> objectIDs, bool all, bool force, const std::string& title)
{
    if(m_reflectionBake)
    {
        m_reflectionBakeMessage = "A bake is already running.";
        return;
    }

    if(m_probeGeneration || m_probeBatchRequest || m_probePublishTask.IsValid() || (m_audioSystem && m_audioSystem->IsAcousticJobRunning()))
    {
        m_reflectionBakeMessage = "Probe generation is running. Bake when it has finished.";
        return;
    }

    if(!m_audioSystem || m_audioSystem->GetSteamAudioStatus() != Audio::SteamAudioStatus::Active)
    {
        m_reflectionBakeMessage = "Baking needs Steam Audio active in this project.";
        return;
    }

    if(!CanEditProbeVolumes(scene))
    {
        m_reflectionBakeMessage = "Reflections can be baked in a stopped scene without pending edits.";
        return;
    }

    // A bake reads every acoustic mesh; one still loading would be left out of the bake.
    if(const auto notReady = AcousticGeometryNotReady(scene))
    {
        m_reflectionBakeMessage = "Cannot bake yet. " + *notReady;
        return;
    }

    if(objectIDs.empty())
    {
        m_reflectionBakeMessage = "The scene has no enabled Probe Volumes.";
        return;
    }

    ReflectionBakeRun run;
    run.all = all;
    run.force = force;
    run.session = *m_historySession;
    run.total = objectIDs.size();
    run.pending.assign(std::make_move_iterator(objectIDs.begin()), std::make_move_iterator(objectIDs.end()));

    if(m_backgroundTasks)
    {
        run.task = m_backgroundTasks->Begin({title, true, InvalidBackgroundTaskId});
        run.task.Start();

        if(all)
        {
            run.task.SetProgress(0, run.total);
        }
    }

    m_reflectionBakeMessage.clear();
    m_reflectionBake = std::move(run);
}

void Editor::StartBakeAll(IDemoScene& scene, bool force)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument())
    {
        m_reflectionBakeMessage = "Open a scene to bake its reflections.";
        return;
    }

    std::vector<std::string> volumes;

    for(const SceneObjectDocument& object : owner->m_runtime->GetFileScene().GetDocument().objects)
    {
        if(object.probeVolume && object.probeVolume->enabled)
        {
            volumes.push_back(object.id);
        }
    }

    StartReflectionBake(scene, std::move(volumes), true, force, force ? "Force Rebake All Reflections" : "Bake All Reflections");
}

BackgroundTaskHandle& Editor::BakeVolumeTask()
{
    ReflectionBakeRun& run = *m_reflectionBake;
    return run.all ? run.current->task : run.task;
}

void Editor::PollReflectionBake(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(!m_reflectionBake)
    {
        return;
    }

    ReflectionBakeRun& run = *m_reflectionBake;
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(run.stopReason.empty())
    {
        if(m_gameState != GameState::Editing)
        {
            run.stopReason = "Play started";
        }
        else if(IsSceneEditingLocked())
        {
            run.stopReason = "A physics simulation started";
        }
        else if(!owner || !RenameSessionMatches(scene, run.session))
        {
            run.stopReason = "The scene changed";
        }
        else if(run.task.IsCancelRequested())
        {
            run.stopReason = "Cancelled";
        }
    }

    // Stopping never finishes a task early: each waits for its job to report that it has stopped.
    if(!run.stopReason.empty())
    {
        run.pending.clear();

        if(run.current && run.current->job)
        {
            run.current->job->Cancel();
        }

        if(run.current && run.current->waitingForProbes && m_probeGeneration)
        {
            m_probeGeneration->job->Cancel();
        }
    }

    if(run.current)
    {
        ReflectionBakeVolume& volume = *run.current;
        const bool volumeCancelled = run.all && volume.task.IsCancelRequested();

        if(volume.waitingForProbes)
        {
            if(volumeCancelled && m_probeGeneration)
            {
                m_probeGeneration->job->Cancel();
            }

            // The generation's own poll stores and commits the probes; this waits for all of that.
            if(m_probeGeneration || m_probeBatchRequest || m_probePublishTask.IsValid())
            {
                return;
            }

            volume.waitingForProbes = false;

            if(!run.stopReason.empty() || volumeCancelled)
            {
                FinishBakeVolume(BakeVolumeOutcome::Cancelled, run.stopReason.empty() ? "Cancelled" : run.stopReason);
            }
            else if(owner)
            {
                // Evaluate again: either the probes are now valid and the bake starts, or this reports why not.
                BeginBakeVolume(scene, *owner, paths, registry);
            }
        }
        else if(volume.job)
        {
            if(volumeCancelled)
            {
                volume.job->Cancel();
            }

            UpdateBakeVolumeTasks();

            if(!volume.job->IsFinished())
            {
                return;
            }

            FinishBakedJob();
        }
        else if(!volume.encoded.empty())
        {
            if(!run.stopReason.empty() || volumeCancelled)
            {
                FinishBakeVolume(BakeVolumeOutcome::Cancelled, (run.stopReason.empty() ? std::string("Cancelled") : run.stopReason) +
                    " before publication. The previous bake is unchanged.");
            }
            else if(!CanEditProbeVolumes(scene))
            {
                BakeVolumeTask().SetStatus("Waiting for the scene to accept edits");
                return;
            }
            else
            {
                const SceneDocument& document = owner->m_runtime->GetFileScene().GetDocument();
                const SceneObjectDocument* object = FindObject(document, volume.objectID);

                if(!object || !object->probeVolume)
                {
                    FinishBakeVolume(BakeVolumeOutcome::Cancelled, "The Probe Volume was removed during baking. Nothing was stored.");
                }
                else
                {
                    // The authoritative check, against the scene as it is at this moment.
                    const ReflectionBakeStatus::Host host = MakeReflectionBakeHost(*owner, scene, paths, registry);
                    const ReflectionBakeStatus::AcousticScene acoustic = ReflectionBakeStatus::CollectScene(document, host);
                    const ReflectionBakeStatus::FreshVolume fresh = ReflectionBakeStatus::EvaluateFresh(document, *object, host, acoustic);
                    std::string why;

                    if(!ReflectionBakeJobs::IsStillCurrent(volume.expected, *object->probeVolume, fresh.status, &why))
                    {
                        FinishBakeVolume(BakeVolumeOutcome::Failed, "Discarded because " + why + ". The previous bake is unchanged; bake again.");
                    }
                    else
                    {
                        BakeVolumeTask().SetStatus("Storing the baked reflections");
                        m_reflectionBakeStoreRequest = ReflectionBakeStoreRequest{volume.objectID, std::move(volume.encoded)};
                        volume.encoded.clear();
                        volume.storing = true;
                    }
                }
            }
        }

        // Storing: OnReflectionBakeStoreCompleted finishes the volume.
        if(m_reflectionBake && m_reflectionBake->current)
        {
            return;
        }
    }

    if(!m_reflectionBake)
    {
        return;
    }

    if(m_reflectionBake->pending.empty())
    {
        FinishReflectionBakeRun();
        return;
    }

    if(owner)
    {
        BeginBakeVolume(scene, *owner, paths, registry);
    }
}

void Editor::BeginBakeVolume(IDemoScene& scene, FileBackedDemoScene& owner, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    ReflectionBakeRun& run = *m_reflectionBake;

    if(!run.current)
    {
        run.current.emplace();
        run.current->objectID = std::move(run.pending.front());
        run.pending.pop_front();
    }

    ReflectionBakeVolume& volume = *run.current;
    const SceneDocument& document = owner.m_runtime->GetFileScene().GetDocument();
    const SceneObjectDocument* object = FindObject(document, volume.objectID);

    if(!object || !object->probeVolume)
    {
        FinishBakeVolume(BakeVolumeOutcome::Cancelled, "The Probe Volume was removed before it could bake.");
        return;
    }

    volume.name = DisplayName(*object);

    if(run.all && !volume.task.IsValid() && m_backgroundTasks)
    {
        volume.task = m_backgroundTasks->Begin({volume.name, true, run.task.GetId()});
        volume.task.Start();
    }

    BackgroundTaskHandle& task = BakeVolumeTask();
    task.SetStatus("Checking what needs baking");

    // Checked again for every volume, in the same main-thread step that takes the geometry below, so no mesh can change between the
    // check and the snapshot. Meshes are published on this thread too.
    if(const auto notReady = AcousticGeometryNotReady(scene))
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "Not baked. " + *notReady);
        return;
    }

    // Authoritative: nothing cached is trusted to start a bake.
    const ReflectionBakeStatus::Host host = MakeReflectionBakeHost(owner, scene, paths, registry);
    const ReflectionBakeStatus::AcousticScene acoustic = ReflectionBakeStatus::CollectScene(document, host);
    const ReflectionBakeStatus::FreshVolume fresh = ReflectionBakeStatus::EvaluateFresh(document, *object, host, acoustic);
    ReflectionBakeJobs::VolumeInputs inputs;
    inputs.objectId = volume.objectID;
    inputs.volume = &*object->probeVolume;
    inputs.probes = fresh.probes ? &*fresh.probes : nullptr;
    inputs.baked = fresh.baked ? &*fresh.baked : nullptr;
    inputs.scene = acoustic.objects;
    inputs.sceneHashes = acoustic.hashes;
    inputs.sources = acoustic.sources;
    ReflectionBakeJobs::Preparation preparation = ReflectionBakeJobs::Prepare(inputs, run.force);

    switch(preparation.outcome)
    {
    case ReflectionBakeJobs::PreparationOutcome::NotRequired:
        FinishBakeVolume(BakeVolumeOutcome::UpToDate, "Disabled: nothing to bake.");
        return;
    case ReflectionBakeJobs::PreparationOutcome::UpToDate:
        FinishBakeVolume(BakeVolumeOutcome::UpToDate, "Already up to date.");
        return;
    case ReflectionBakeJobs::PreparationOutcome::NeedsProbes:
        if(volume.probesRequested)
        {
            FinishBakeVolume(BakeVolumeOutcome::Failed, std::string("The probes are still not usable (") + ProbeVolumes::ToString(fresh.probeStatus) +
                (m_probeEditStatus.empty() ? std::string(")") : "): " + m_probeEditStatus));
            return;
        }

        // Missing or stale probes would bake against the wrong layout, so the ordinary generation runs first.
        volume.probesRequested = true;
        m_probeEditStatus.clear();

        if(!GenerateProbes(scene, owner, *object, task.GetId()))
        {
            FinishBakeVolume(BakeVolumeOutcome::Failed, "The probes could not be generated: " + m_probeEditStatus);
            return;
        }

        volume.waitingForProbes = true;
        task.SetStatus(std::string("Generating probes first (") + ProbeVolumes::ToString(fresh.probeStatus) + ")");
        task.SetIndeterminate();
        return;
    case ReflectionBakeJobs::PreparationOutcome::Bake:
        break;
    }

    for(std::size_t index = 0; index < preparation.expected.layers.size(); ++index)
    {
        if(preparation.expected.sources[index].baked && m_backgroundTasks)
        {
            volume.layers.push_back(m_backgroundTasks->Begin({LayerName(preparation.expected.layers[index], document), false, task.GetId()}));
        }
    }

    volume.expected = std::move(preparation.expected);
    volume.reused = preparation.reused;
    volume.job = m_audioSystem ? m_audioSystem->StartReflectionBake(std::move(preparation.request)) : nullptr;

    if(!volume.job)
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "Steam Audio is not active, or another acoustic job is still stopping.");
        return;
    }

    UpdateBakeVolumeTasks();
}

// Real work units only: objects while the scene builds, probes while a layer bakes. The short stages in between are indeterminate.
void Editor::UpdateBakeVolumeTasks()
{
    ReflectionBakeRun& run = *m_reflectionBake;
    ReflectionBakeVolume& volume = *run.current;
    const Audio::ReflectionBakeJob& job = *volume.job;
    BackgroundTaskHandle& task = BakeVolumeTask();
    const Audio::ReflectionBakeStage stage = job.GetStage();
    const std::size_t layers = volume.layers.size();
    const std::size_t done = job.GetLayersDone();
    const std::uint64_t probes = job.GetTotal();
    std::string status;

    switch(stage)
    {
    case Audio::ReflectionBakeStage::BuildingScene:
        status = "Building the acoustic scene";
        task.SetProgress((std::min)(job.GetDone(), job.GetTotal()), job.GetTotal());
        break;
    case Audio::ReflectionBakeStage::LoadingProbes:
        status = "Loading the probes";
        task.SetIndeterminate();
        break;
    case Audio::ReflectionBakeStage::Baking:
        status = "Baking layer " + std::to_string(job.GetLayer() + 1) + " of " + std::to_string(layers);

        if(probes > 0 && layers > 0)
        {
            const std::uint64_t total = probes * layers;
            task.SetProgress((std::min)(done * probes + (std::min)(job.GetDone(), probes), total), total);
        }

        break;
    case Audio::ReflectionBakeStage::Validating:
        status = "Validating the layers";
        task.SetIndeterminate();
        break;
    case Audio::ReflectionBakeStage::Serializing:
        status = "Serializing and checking the batch";
        task.SetIndeterminate();
        break;
    default:
        status = "Starting";
        task.SetIndeterminate();
        break;
    }

    if(volume.reused > 0)
    {
        status += " (" + std::to_string(volume.reused) + " valid layers reused)";
    }

    task.SetStatus(job.IsCancelRequested() ? std::string("Cancelling: stops at the next probe") : status);

    for(std::size_t index = 0; index < layers; ++index)
    {
        BackgroundTaskHandle& layer = volume.layers[index];

        if(index < done)
        {
            layer.Start();
            layer.SetProgress(1, 1);
            // Completed only once the whole asset is published.
            layer.SetStatus("Baked; waiting for the other layers and publication");
        }
        else if(index == job.GetLayer() && stage == Audio::ReflectionBakeStage::Baking)
        {
            layer.Start();
            layer.SetStatus("Baking");
            layer.SetProgress((std::min)(job.GetDone(), probes), probes);
        }
    }
}

void Editor::FinishBakedJob()
{
    ReflectionBakeRun& run = *m_reflectionBake;
    ReflectionBakeVolume& volume = *run.current;
    const std::shared_ptr<Audio::ReflectionBakeJob> job = std::exchange(volume.job, nullptr);
    const Audio::ReflectionBakeResult& result = job->GetResult();

    // Cancelled data is partial; it went with the job's private batch.
    if(result.cancelled || job->IsCancelRequested())
    {
        FinishBakeVolume(BakeVolumeOutcome::Cancelled, (run.stopReason.empty() ? std::string("Cancelled") : run.stopReason) +
            ". The previous bake is unchanged.");
        return;
    }

    if(!result.success)
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "Baking failed: " + result.error + ". The previous bake is unchanged.");
        return;
    }

    ReflectionBakeJobs::Assembly assembly = ReflectionBakeJobs::Assemble(volume.expected, result);

    if(!assembly.success)
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "The bake could not be validated: " + assembly.error + ". The previous bake is unchanged.");
        return;
    }

    volume.encoded = std::move(assembly.encoded);
    volume.rayTracer = result.rayTracer;
    BakeVolumeTask().SetStatus("Publishing");
    BakeVolumeTask().SetIndeterminate();
}

// Engine has stored the asset (or failed to). The reference changes only through scene history, so undo restores the previous bake.
void Editor::OnReflectionBakeStoreCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error)
{
    if(!m_reflectionBake || !m_reflectionBake->current || m_reflectionBake->current->objectID != objectID || !m_reflectionBake->current->storing)
    {
        return;
    }

    ReflectionBakeVolume& volume = *m_reflectionBake->current;
    volume.storing = false;

    if(!error.empty())
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "Could not store the baked reflections: " + error + ". The previous bake is unchanged.");
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;
    std::string lookupError;
    const auto selection = owner ? owner->GetObjectMetadata(objectID, &lookupError) : std::nullopt;

    if(!selection || !owner->CaptureTriggerColliderInspection(objectID, object) || !object.probeVolume)
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "The Probe Volume was removed while its bake was stored. The scene is unchanged.");
        return;
    }

    if(!(*object.probeVolume == volume.expected.volume))
    {
        FinishBakeVolume(BakeVolumeOutcome::Failed, "The Probe Volume changed while its bake was stored. The scene is unchanged.");
        return;
    }

    ProbeVolumeComponent after = *object.probeVolume;
    after.bakedReflections = reference;
    const std::string probeStatus = m_probeEditStatus;

    if(!CommitObjectProbeVolume(scene, *selection, object.probeVolume, after))
    {
        const std::string why = m_probeEditStatus;
        m_probeEditStatus = probeStatus;
        FinishBakeVolume(BakeVolumeOutcome::Failed, "The scene did not accept the bake: " + why);
        return;
    }

    m_probeEditStatus = probeStatus;
    const std::size_t baked = volume.layers.size();
    std::string message = "Baked " + std::to_string(baked) + (baked == 1 ? " layer" : " layers");

    if(volume.reused > 0)
    {
        message += ", reused " + std::to_string(volume.reused);
    }

    // The backend actually used: the project's choice, or Default when Embree fell back for this session.
    message += std::string(" (") + Audio::ToString(volume.rayTracer) + " ray tracer)";
    Log::Info("Reflection bake of " + volume.name + ": " + message + ".");
    FinishBakeVolume(BakeVolumeOutcome::Baked, message + ".");
}

void Editor::FinishBakeVolume(BakeVolumeOutcome outcome, const std::string& message)
{
    ReflectionBakeRun& run = *m_reflectionBake;
    ReflectionBakeVolume& volume = *run.current;

    for(BackgroundTaskHandle& layer : volume.layers)
    {
        switch(outcome)
        {
        case BakeVolumeOutcome::Baked:
        case BakeVolumeOutcome::UpToDate:
            layer.Complete();
            break;
        case BakeVolumeOutcome::Failed:
            layer.MarkCancelled("Not published: the volume's bake failed.");
            break;
        case BakeVolumeOutcome::Cancelled:
            layer.MarkCancelled("Not published.");
            break;
        }
    }

    switch(outcome)
    {
    case BakeVolumeOutcome::Baked:
        ++run.baked;
        m_reflectionBakeFailures.erase(volume.objectID);
        break;
    case BakeVolumeOutcome::UpToDate:
        ++run.upToDate;
        m_reflectionBakeFailures.erase(volume.objectID);
        break;
    case BakeVolumeOutcome::Failed:
        ++run.failed;
        m_reflectionBakeFailures[volume.objectID] = message;

        if(run.firstFailure.empty())
        {
            run.firstFailure = (run.all ? volume.name + ": " : std::string()) + message;
        }

        break;
    case BakeVolumeOutcome::Cancelled:
        ++run.cancelled;
        break;
    }

    // A single-volume run's task is the run's own: FinishReflectionBakeRun reports it.
    if(run.all)
    {
        switch(outcome)
        {
        case BakeVolumeOutcome::Baked:
        case BakeVolumeOutcome::UpToDate:
            volume.task.Complete(message);
            break;
        case BakeVolumeOutcome::Failed:
            volume.task.Fail(message);
            break;
        case BakeVolumeOutcome::Cancelled:
            volume.task.MarkCancelled(message);
            break;
        }

        run.task.SetProgress(run.baked + run.upToDate + run.failed + run.cancelled, run.total);
    }

    m_reflectionBakeMessage = (run.all ? volume.name + ": " : std::string()) + message;
    run.current.reset();
    m_reflectionBakeStatusNext = {};
}

void Editor::FinishReflectionBakeRun()
{
    ReflectionBakeRun& run = *m_reflectionBake;
    std::string summary = m_reflectionBakeMessage;

    if(run.all)
    {
        summary = std::to_string(run.baked) + " baked, " + std::to_string(run.upToDate) + " up to date";

        if(run.failed > 0)
        {
            summary += ", " + std::to_string(run.failed) + " failed (first: " + run.firstFailure + ")";
        }

        if(run.cancelled > 0)
        {
            summary += ", " + std::to_string(run.cancelled) + " cancelled";
        }

        const std::size_t notStarted = run.total - (std::min)(run.total, run.baked + run.upToDate + run.failed + run.cancelled);

        if(notStarted > 0)
        {
            summary += ", " + std::to_string(notStarted) + " not started";
        }

        summary += ".";
    }

    if(!run.stopReason.empty())
    {
        run.task.MarkCancelled(run.stopReason + ". " + summary);
        m_reflectionBakeMessage = run.stopReason + ". " + summary;
    }
    else if(run.failed > 0)
    {
        run.task.Fail(summary);
        m_reflectionBakeMessage = summary;
    }
    else
    {
        run.task.Complete(summary);
        m_reflectionBakeMessage = summary;
    }

    m_reflectionBake.reset();
    m_reflectionBakeStatusNext = {};
}

void Editor::StopAcousticWork(std::string_view reason)
{
    const std::string message(reason);

    if(m_probeGeneration)
    {
        m_probeGeneration->task.MarkCancelled(message);
        m_probeGeneration.reset();
    }

    m_probePublishTask.MarkCancelled(message);
    m_probePublishTask = {};
    m_probeBatchRequest.reset();
    m_reflectionBakeStoreRequest.reset();
    m_pathingStoreRequest.reset();

    if(m_pathingBake)
    {
        m_pathingBake->job.reset();
        FinishPathingBake(false, false, message);
    }

    if(m_reflectionBake)
    {
        m_reflectionBake->stopReason = message;
        m_reflectionBake->pending.clear();

        if(m_reflectionBake->current)
        {
            m_reflectionBake->current->job.reset();
            FinishBakeVolume(BakeVolumeOutcome::Cancelled, message);
        }

        FinishReflectionBakeRun();
    }
}

const char* Editor::ReflectionBakeWarning(std::string_view objectID) const
{
    const ReflectionBakeStatus::Cache::Entry* entry = m_reflectionBakeStatus.Find(objectID);

    if(!entry || !ReflectionBakes::ShowsStaleWarning(entry->status))
    {
        return nullptr;
    }

    return entry->status.state == ReflectionBakes::VolumeState::Partial ? "Baked reflections are partly out of date. Bake again." :
        "Baked reflections are out of date. Bake again.";
}

void Editor::DrawReflectionBakeControls(IDemoScene& scene, const SceneObjectMetadata& selection, const SceneObjectDocument& object, bool blocked)
{
    const ProbeVolumeComponent& volume = *object.probeVolume;
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner && owner->m_runtime && owner->m_runtime->GetFileScene().HasDocument() ?
        &owner->m_runtime->GetFileScene().GetDocument() : nullptr;
    ImGui::SeparatorText("Baked Reflections");

    const ReflectionBakeStatus::Cache::Entry* entry = m_reflectionBakeStatus.Find(object.id);
    const bool current = m_reflectionBake && m_reflectionBake->current && m_reflectionBake->current->objectID == object.id;
    const bool queued = m_reflectionBake && std::find(m_reflectionBake->pending.begin(), m_reflectionBake->pending.end(), object.id) !=
        m_reflectionBake->pending.end();
    const auto failure = m_reflectionBakeFailures.find(object.id);
    const ReflectionBakes::JobState jobState = current ? ReflectionBakes::JobState::Baking :
        failure != m_reflectionBakeFailures.end() ? ReflectionBakes::JobState::Failed : ReflectionBakes::JobState::None;
    const ReflectionBakes::DisplayState display = entry ? ReflectionBakes::Present(entry->status, jobState) :
        (current ? ReflectionBakes::DisplayState::Baking : ReflectionBakes::DisplayState::Missing);

    switch(display)
    {
    case ReflectionBakes::DisplayState::Valid:
        ImGui::TextColored(ValidColor, "Status: %s", ToString(display));
        break;
    case ReflectionBakes::DisplayState::Partial:
    case ReflectionBakes::DisplayState::Stale:
        ImGui::TextColored(WarningColor, "Status: %s", ToString(display));
        break;
    case ReflectionBakes::DisplayState::Failed:
        ImGui::TextColored(ErrorColor, "Status: %s", ToString(display));
        break;
    case ReflectionBakes::DisplayState::Baking:
        ImGui::Text("Status: %s", ToString(display));
        break;
    default:
        ImGui::TextDisabled("Status: %s%s", ToString(display), queued ? " (queued)" : "");
        break;
    }

    if(entry && entry->status.state != ReflectionBakes::VolumeState::NotRequired)
    {
        const ReflectionBakes::VolumeStatus& status = entry->status;
        const std::size_t valid = static_cast<std::size_t>(std::count_if(status.layers.begin(), status.layers.end(),
            [](const ReflectionBakes::LayerStatus& layer)
        {
            return layer.state == ReflectionBakes::LayerState::Valid;
        }));
        ImGui::Text("%zu of %zu layers valid (listener reverb and %zu static sources).", valid, status.layers.size(),
            status.layers.empty() ? std::size_t{0} : status.layers.size() - 1);

        if(status.assetProblem != ReflectionBakes::StaleReason::None)
        {
            ImGui::TextColored(WarningColor, "%s.", ReflectionBakes::ToString(status.assetProblem));
        }
        else if(!status.probesReady)
        {
            ImGui::TextWrapped("The probes are %s: baking generates them first.", ProbeVolumes::ToString(entry->probeStatus));
        }

        // A never-baked volume lists nothing: missing layers there are expected, not a warning.
        if(status.state == ReflectionBakes::VolumeState::Partial || status.state == ReflectionBakes::VolumeState::Stale)
        {
            std::size_t shown = 0;
            std::size_t hidden = 0;

            for(const ReflectionBakes::LayerStatus& layer : status.layers)
            {
                if(layer.state == ReflectionBakes::LayerState::Valid)
                {
                    continue;
                }

                if(shown == ShownLayerReasons)
                {
                    ++hidden;
                    continue;
                }

                const std::string name = document ? LayerName(layer.required, *document) : layer.required.sourceObjectId;
                ImGui::BulletText("%s: %s", name.c_str(), ReflectionBakes::ToString(layer.reason));
                ++shown;
            }

            if(hidden > 0)
            {
                ImGui::TextDisabled("and %zu more.", hidden);
            }
        }

        if(!status.unused.empty())
        {
            ImGui::TextDisabled("%zu baked layers belong to sources that no longer need one; the next bake removes them.", status.unused.size());
        }
    }

    if(failure != m_reflectionBakeFailures.end() && !current)
    {
        ImGui::TextColored(ErrorColor, "%s", failure->second.c_str());
    }

    if(current)
    {
        const ReflectionBakeVolume& baking = *m_reflectionBake->current;

        if(baking.job)
        {
            const Audio::ReflectionBakeJob& job = *baking.job;
            const std::uint64_t total = job.GetTotal();
            const std::string label = job.GetStage() == Audio::ReflectionBakeStage::Baking ? "Layer " + std::to_string(job.GetLayer() + 1) +
                " of " + std::to_string(baking.layers.size()) : std::string("Preparing");
            ImGui::ProgressBar(total > 0 ? static_cast<float>(static_cast<double>((std::min)(job.GetDone(), total)) / static_cast<double>(total)) : 0.0f,
                ImVec2(-1.0f, 0.0f), label.c_str());
        }
        else if(baking.waitingForProbes)
        {
            ImGui::TextUnformatted("Generating probes first.");
        }

        const bool cancelling = !m_reflectionBake->stopReason.empty() || (baking.job && baking.job->IsCancelRequested());

        if(cancelling)
        {
            ImGui::TextDisabled("Cancelling: the bake stops at its next probe.");
        }
        else if(ImGui::Button("Cancel Bake"))
        {
            // One volume of Bake All stops alone; a single-volume run stops whole.
            if(m_reflectionBake->all && baking.job)
            {
                baking.job->Cancel();
            }
            else
            {
                m_reflectionBake->stopReason = "Cancelled";
            }
        }
    }

    const bool busy = m_reflectionBake.has_value() || m_probeGeneration.has_value() || m_probeBatchRequest.has_value() || m_probePublishTask.IsValid();
    // Not while a bake runs: its meshes were checked when it took them.
    const auto notReady = busy ? std::nullopt : AcousticGeometryNotReady(scene);
    const bool canBake = m_audioSystem && m_audioSystem->GetSteamAudioStatus() == Audio::SteamAudioStatus::Active && !notReady;

    if(notReady)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
        ImGui::TextWrapped("%s", notReady->c_str());
        ImGui::PopStyleColor();
    }

    ImGui::BeginDisabled(blocked || !canBake || busy || !volume.enabled);

    if(ImGui::Button("Bake Reflections"))
    {
        StartReflectionBake(scene, {object.id}, false, false, "Bake Reflections: " + DisplayName(object));
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked || busy || volume.bakedReflections.empty());

    if(ImGui::Button("Clear Bake"))
    {
        // Only the reference goes, so undo restores the bake. The file waits for Unused Probe Files.
        ProbeVolumeComponent cleared = volume;
        cleared.bakedReflections.clear();

        if(CommitObjectProbeVolume(scene, selection, object.probeVolume, cleared))
        {
            m_reflectionBakeFailures.erase(object.id);
            m_reflectionBakeMessage = "Bake cleared. Undo restores it.";
            m_reflectionBakeStatusNext = {};
        }
    }

    ImGui::EndDisabled();
    ImGui::BeginDisabled(blocked || !canBake || busy);

    if(ImGui::Button("Bake All Reflections"))
    {
        StartBakeAll(scene, false);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bakes every enabled Probe Volume in the scene, reusing valid layers and skipping volumes that are up to date.");
    }

    ImGui::SameLine();

    if(ImGui::Button("Force Rebake All"))
    {
        StartBakeAll(scene, true);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Rebakes every layer of every enabled Probe Volume, reusing nothing. Valid probes are not regenerated.");
    }

    ImGui::EndDisabled();

    if(!canBake)
    {
        ImGui::TextDisabled("Baking needs Steam Audio active in this project.");
    }

    if(ImGui::TreeNode("Bake Settings"))
    {
        ReflectionBakeSettings& settings = m_probeDraft.bakeSettings;
        bool commit = false;
        ImGui::BeginDisabled(blocked);
        const std::uint32_t minimumRays = ReflectionBakeSettings::MinimumRays;
        const std::uint32_t maximumRays = ReflectionBakeSettings::MaximumRays;
        const std::uint32_t minimumSamples = ReflectionBakeSettings::MinimumDiffuseSamples;
        const std::uint32_t maximumSamples = ReflectionBakeSettings::MaximumDiffuseSamples;
        const std::uint32_t minimumBounces = ReflectionBakeSettings::MinimumBounces;
        const std::uint32_t maximumBounces = ReflectionBakeSettings::MaximumBounces;
        ImGui::DragScalar("Rays", ImGuiDataType_U32, &settings.rays, 64.0f, &minimumRays, &maximumRays, "%u", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragScalar("Diffuse Samples", ImGuiDataType_U32, &settings.diffuseSamples, 8.0f, &minimumSamples, &maximumSamples, "%u",
            ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragScalar("Bounces", ImGuiDataType_U32, &settings.bounces, 0.25f, &minimumBounces, &maximumBounces, "%u", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragFloat("Irradiance Minimum (m)", &settings.irradianceMinimumDistance, 0.01f, ReflectionBakeSettings::MinimumIrradianceDistance,
            ReflectionBakeSettings::MaximumIrradianceDistance, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;

        if(ImGui::Button("Reset to Defaults"))
        {
            settings = ReflectionBakeSettings{};
            commit = true;
        }

        ImGui::EndDisabled();
        ImGui::TextDisabled("Order %u and duration %.1f s follow the simulator. Changing a setting makes only this volume's layers stale.",
            ReflectionBakes::BakeOrder, static_cast<double>(ReflectionBakes::BakeDuration));

        if(commit && !(settings == volume.bakeSettings))
        {
            std::string error;

            if(!ValidateReflectionBakeSettings(settings, &error))
            {
                m_probeEditStatus = "Bake settings refused: " + error + ".";
                settings = volume.bakeSettings;
            }
            else
            {
                ProbeVolumeComponent after = volume;
                after.bakeSettings = settings;

                if(CommitObjectProbeVolume(scene, selection, object.probeVolume, after))
                {
                    m_reflectionBakeStatusNext = {};
                }
                else
                {
                    settings = volume.bakeSettings;
                }
            }
        }

        ImGui::TreePop();
    }

    if(!m_reflectionBakeMessage.empty())
    {
        ImGui::TextWrapped("%s", m_reflectionBakeMessage.c_str());
    }
}
