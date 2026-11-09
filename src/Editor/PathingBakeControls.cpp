#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"
#include "Editor/ProbeSceneAccess.h"
#include "Engine/PathingBakes.h"
#include "Engine/ReflectionBakeStatus.h"
#include "assets/AcousticMaterial.h"
#include "assets/BakedPathingAsset.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

// M9.6: the Editor's side of pathing baking, one Probe Volume at a time. The same discipline as reflection baking:
//
//   evaluate afresh -> bake on the acoustic job thread -> assemble and verify the asset -> wait until the scene accepts edits ->
//   evaluate afresh again -> Engine stores the asset -> commit the reference through scene history.
//
// The bake is one uninterruptible Steam Audio call: 4.8.1 crashes on iplPathBakerCancelBake (M9.4), so Engine never calls it. Cancelling,
// Play or closing the scene waits for that call to return and then discards the result. Nothing is published unless the last evaluation still
// matches what was baked, and the previous asset and reference are never touched by a failed, cancelled or discarded bake.
namespace
{
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);
    const ImVec4 ValidColor(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 ErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
    // The status reads every acoustic mesh and decodes the asset, so it is refreshed only for the volume being shown, and not often.
    constexpr auto StatusRefreshInterval = std::chrono::seconds(1);
    // M9.4 measurements: the pathing layer is about 5.25 bytes per probe pair, and every pair is tested for visibility.
    constexpr double LayerBytesPerProbePair = 5.25;
    constexpr double GuidanceBytes = 256.0 * 1024.0 * 1024.0;
    constexpr double GuidanceWarningShare = 0.75;

    const char* ToString(Audio::PathingBakeStage stage) noexcept
    {
        switch(stage)
        {
        case Audio::PathingBakeStage::BuildingScene:
            return "Building the acoustic scene";
        case Audio::PathingBakeStage::LoadingProbes:
            return "Loading the probes";
        case Audio::PathingBakeStage::Baking:
            return "Baking (cannot be interrupted)";
        case Audio::PathingBakeStage::Serializing:
            return "Serializing and checking the batch";
        default:
            return "Starting";
        }
    }

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

    const char* ToString(PathingBakes::State state) noexcept
    {
        switch(state)
        {
        case PathingBakes::State::NoProbes:
            return "No current probes";
        case PathingBakes::State::NotBaked:
            return "Not baked";
        case PathingBakes::State::Missing:
            return "Missing asset";
        case PathingBakes::State::Corrupt:
            return "Corrupt asset";
        case PathingBakes::State::Stale:
            return "Stale";
        case PathingBakes::State::Valid:
            return "Valid";
        }

        return "Unknown";
    }

    // What the volume's settings and probes would mean for the bake, from the M9.4 measurements. Never changes anything.
    void DrawPathingGuidance(std::size_t probes, const PathingBakeSettings& settings)
    {
        if(probes == 0)
        {
            ImGui::TextDisabled("Generate probes to see the bake estimate.");
            return;
        }

        const double pairs = static_cast<double>(probes) * static_cast<double>(probes);
        const double bytes = pairs * LayerBytesPerProbePair;
        ImGui::Text("%zu probes: about %.1f MiB of pathing data.", probes, bytes / (1024.0 * 1024.0));

        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Every probe pair is stored and tested, so size and bake time grow with the square of the probe count.\n"
                "Measured: 1,258 probes made 8.3 MB and baked in about 1 s at 4 samples, 11.6 s at 64 samples.\n"
                "Bake time grows with Samples squared as well.");
        }

        if(bytes >= GuidanceBytes)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ErrorColor);
            ImGui::TextWrapped("Over 256 MiB: this is more than pathing needs and will be slow to bake and load. Use a larger probe spacing, "
                "or split the area into several volumes.");
            ImGui::PopStyleColor();
        }
        else if(bytes >= GuidanceBytes * GuidanceWarningShare)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
            ImGui::TextWrapped("Approaching 256 MiB. Consider a larger probe spacing, or splitting the area into several volumes.");
            ImGui::PopStyleColor();
        }

        if(settings.samples > 16)
        {
            ImGui::TextDisabled("%u samples: %u rays per probe pair; bake time grows with the square of Samples.", settings.samples,
                settings.samples * settings.samples);
        }
    }
}

PathingBakes::Inputs Editor::CurrentPathingInputs(const FileBackedDemoScene& owner, const IDemoScene& scene, const SceneObjectDocument& object,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::optional<ProbeBatchAssets::Data>& probes,
    std::vector<Audio::ProbeGeometryObject>* geometry) const
{
    const SceneDocument& document = owner.m_runtime->GetFileScene().GetDocument();
    const ReflectionBakeStatus::Host host = MakeReflectionBakeHost(owner, scene, paths, registry);
    const ReflectionBakeStatus::AcousticScene acoustic = ReflectionBakeStatus::CollectScene(document, host);
    ReflectionBakeStatus::FreshVolume fresh = ReflectionBakeStatus::EvaluateFresh(document, object, host, acoustic);
    probes = std::move(fresh.probes);

    if(geometry)
    {
        geometry->clear();
        geometry->reserve(acoustic.objects.size());

        for(const ReflectionBakes::AcousticObject& entry : acoustic.objects)
        {
            geometry->push_back({entry.mesh, entry.world});
        }
    }

    PathingBakes::Inputs inputs;
    inputs.probeReference = object.probeVolume->probeBatch;
    inputs.probes = probes ? &*probes : nullptr;
    inputs.geometryHash = acoustic.hashes.geometry;
    inputs.settings = object.probeVolume->pathingSettings;
    return inputs;
}

void Editor::StartPathingBake(IDemoScene& scene, const SceneObjectDocument& object, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(m_pathingBake || m_reflectionBake)
    {
        m_pathingMessage = "A bake is already running.";
        return;
    }

    if(m_probeGeneration || m_probeBatchRequest || m_probePublishTask.IsValid() || (m_audioSystem && m_audioSystem->IsAcousticJobRunning()))
    {
        m_pathingMessage = "Another acoustic job is running. Bake when it has finished.";
        return;
    }

    if(!m_audioSystem || m_audioSystem->GetSteamAudioStatus() != Audio::SteamAudioStatus::Active)
    {
        m_pathingMessage = "Baking needs Steam Audio active in this project.";
        return;
    }

    if(!CanEditProbeVolumes(scene))
    {
        m_pathingMessage = "Pathing can be baked in a stopped scene without pending edits.";
        return;
    }

    // The same readiness gate as reflections: a bake reads every acoustic mesh, and one still loading would be left out.
    if(const auto notReady = AcousticGeometryNotReady(scene))
    {
        m_pathingMessage = "Cannot bake yet. " + *notReady;
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument() || !object.probeVolume)
    {
        m_pathingMessage = "Open a scene to bake its pathing.";
        return;
    }

    if(!object.probeVolume->enabled || !object.probeVolume->pathingEnabled)
    {
        m_pathingMessage = "Enable the volume and its pathing first.";
        return;
    }

    // Authoritative: nothing cached is trusted to start a bake.
    PathingBakeRun run;
    std::optional<ProbeBatchAssets::Data> probes;
    std::vector<Audio::ProbeGeometryObject> geometry;
    PathingBakes::Inputs inputs = CurrentPathingInputs(*owner, scene, object, paths, registry, probes, &geometry);

    if(!probes)
    {
        m_pathingMessage = "The probes are missing or out of date. Generate probes first.";
        return;
    }

    run.probes = std::move(*probes);
    inputs.probes = &run.probes;
    std::optional<Audio::PathingBakeRequest> request = PathingBakes::MakeRequest(inputs, std::move(geometry));

    if(!request)
    {
        m_pathingMessage = "The probes hold nothing to bake.";
        return;
    }

    run.job = m_audioSystem->StartPathingBake(std::move(*request));

    if(!run.job)
    {
        m_pathingMessage = "Steam Audio is not active, or another acoustic job is still stopping.";
        return;
    }

    run.objectID = object.id;
    run.name = DisplayName(object);
    run.session = *m_historySession;
    run.volume = *object.probeVolume;
    run.inputs = inputs;

    if(m_backgroundTasks)
    {
        // Not cancellable from the task window: the SDK call runs to its end whatever is asked.
        run.task = m_backgroundTasks->Begin({"Bake Pathing: " + run.name, true, InvalidBackgroundTaskId});
        run.task.Start();
    }

    m_pathingMessage.clear();
    m_pathingBake = std::move(run);
    // The run's probes moved with it; point the inputs at their final place.
    m_pathingBake->inputs.probes = &m_pathingBake->probes;
}

void Editor::PollPathingBake(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(!m_pathingBake)
    {
        return;
    }

    PathingBakeRun& run = *m_pathingBake;
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

    if(run.job)
    {
        // Only marks it: the baker checks before and after its one SDK call, never during it.
        if(!run.stopReason.empty())
        {
            run.job->Cancel();
        }

        const Audio::PathingBakeStage stage = run.job->GetStage();
        const std::uint64_t total = run.job->GetTotal();

        if(stage == Audio::PathingBakeStage::Baking && total > 0)
        {
            run.task.SetProgress((std::min)(run.job->GetDone(), total), total);
        }
        else
        {
            run.task.SetIndeterminate();
        }

        if(run.stopReason.empty())
        {
            run.task.SetStatus(ToString(stage));
        }
        else
        {
            run.task.SetStatus(run.stopReason + ": waiting for Steam Audio's bake call to return, then discarding it");
        }

        if(!run.job->IsFinished())
        {
            return;
        }

        const std::shared_ptr<Audio::PathingBakeJob> job = std::exchange(run.job, nullptr);
        const Audio::PathingBakeResult& result = job->GetResult();

        if(!run.stopReason.empty() || result.cancelled || job->IsCancelRequested())
        {
            FinishPathingBake(false, false, (run.stopReason.empty() ? std::string("Cancelled") : run.stopReason) +
                ". The bake was discarded; the previous bake is unchanged.");
            return;
        }

        if(!result.success)
        {
            FinishPathingBake(false, true, "Baking failed: " + result.error + ". The previous bake is unchanged.");
            return;
        }

        PathingBakes::Assembly assembly = PathingBakes::Assemble(run.inputs, result);

        if(!assembly.error.empty())
        {
            FinishPathingBake(false, true, "The bake could not be validated: " + assembly.error + ". The previous bake is unchanged.");
            return;
        }

        run.encoded = std::move(assembly.encoded);
        run.task.SetStatus("Publishing");
        run.task.SetIndeterminate();
    }

    if(run.storing || run.encoded.empty())
    {
        return;
    }

    if(!run.stopReason.empty())
    {
        FinishPathingBake(false, false, run.stopReason + " before publication. The previous bake is unchanged.");
        return;
    }

    if(!CanEditProbeVolumes(scene))
    {
        run.task.SetStatus("Waiting for the scene to accept edits");
        return;
    }

    const SceneObjectDocument* object = FindObject(owner->m_runtime->GetFileScene().GetDocument(), run.objectID);

    if(!object || !object->probeVolume)
    {
        FinishPathingBake(false, false, "The Probe Volume was removed during baking. Nothing was stored.");
        return;
    }

    // The authoritative check, against the scene as it is at this moment: the same probes, geometry and settings as were baked.
    std::optional<ProbeBatchAssets::Data> probes;
    const PathingBakes::Inputs now = CurrentPathingInputs(*owner, scene, *object, paths, registry, probes, nullptr);
    const char* why = nullptr;

    if(!(object->probeVolume->pathingSettings == run.volume.pathingSettings))
    {
        why = "the pathing settings changed";
    }
    else if(!object->probeVolume->pathingEnabled)
    {
        why = "pathing was disabled";
    }
    else if(!now.probes)
    {
        why = "the probes are no longer current";
    }
    else if(PathingBakes::Fingerprint(now) != PathingBakes::Fingerprint(run.inputs))
    {
        why = "the probes or the acoustic geometry changed";
    }

    if(why)
    {
        FinishPathingBake(false, true, std::string("Discarded because ") + why + ". The previous bake is unchanged; bake again.");
        return;
    }

    run.task.SetStatus("Storing the baked pathing");
    m_pathingStoreRequest = ReflectionBakeStoreRequest{run.objectID, std::move(run.encoded)};
    run.encoded.clear();
    run.storing = true;
}

// Engine has stored the asset (or failed to). The reference changes only through scene history, so undo restores the previous bake.
void Editor::OnPathingStoreCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error)
{
    if(!m_pathingBake || m_pathingBake->objectID != objectID || !m_pathingBake->storing)
    {
        return;
    }

    PathingBakeRun& run = *m_pathingBake;
    run.storing = false;

    if(!error.empty())
    {
        FinishPathingBake(false, true, "Could not store the baked pathing: " + error + ". The previous bake is unchanged.");
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;
    std::string lookupError;
    const auto selection = owner ? owner->GetObjectMetadata(objectID, &lookupError) : std::nullopt;

    if(!selection || !owner->CaptureTriggerColliderInspection(objectID, object) || !object.probeVolume)
    {
        FinishPathingBake(false, true, "The Probe Volume was removed while its bake was stored. The scene is unchanged.");
        return;
    }

    if(!(*object.probeVolume == run.volume))
    {
        FinishPathingBake(false, true, "The Probe Volume changed while its bake was stored. The scene is unchanged.");
        return;
    }

    ProbeVolumeComponent after = *object.probeVolume;
    after.bakedPathing = reference;
    const std::string probeStatus = m_probeEditStatus;

    if(!CommitObjectProbeVolume(scene, *selection, object.probeVolume, after))
    {
        const std::string why = m_probeEditStatus;
        m_probeEditStatus = probeStatus;
        FinishPathingBake(false, true, "The scene did not accept the bake: " + why);
        return;
    }

    m_probeEditStatus = probeStatus;
    const std::string message = "Baked pathing for " + std::to_string(run.probes.probes.size()) + " probes.";
    Log::Info("Pathing bake of " + run.name + ": " + message);
    FinishPathingBake(true, false, message);
}

void Editor::FinishPathingBake(bool success, bool failed, const std::string& message)
{
    PathingBakeRun& run = *m_pathingBake;

    if(success)
    {
        run.task.Complete(message);
    }
    else if(failed)
    {
        run.task.Fail(message);
    }
    else
    {
        run.task.MarkCancelled(message);
    }

    m_pathingStatus.erase(run.objectID);
    m_pathingMessage = message;
    m_pathingBake.reset();
}

// Display only: refreshed for the shown volume, at most once a second, and not while it bakes. Bakes and publication evaluate afresh.
const Editor::PathingStatusEntry& Editor::RefreshPathingStatus(const IDemoScene& scene, const SceneObjectDocument& object, const ProjectPaths& paths,
    const ProjectAssetRegistry& registry)
{
    PathingStatusEntry& entry = m_pathingStatus[object.id];
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto now = std::chrono::steady_clock::now();
    const bool baking = m_pathingBake && m_pathingBake->objectID == object.id;

    if(baking || now < entry.next || !owner || !owner->m_runtime || !owner->m_runtime->GetFileScene().HasDocument() ||
        m_gameState != GameState::Editing)
    {
        return entry;
    }

    entry.next = now + StatusRefreshInterval;
    std::optional<ProbeBatchAssets::Data> probes;
    const PathingBakes::Inputs inputs = CurrentPathingInputs(*owner, scene, object, paths, registry, probes, nullptr);
    std::optional<BakedPathingAssets::Resolution> asset;

    if(!object.probeVolume->bakedPathing.empty())
    {
        asset = BakedPathingAssets::Resolve(object.probeVolume->bakedPathing, paths, registry);
    }

    entry.status = PathingBakes::Evaluate(inputs, asset ? &*asset : nullptr);
    entry.probes = probes ? probes->probes.size() : 0;
    return entry;
}

void Editor::DrawPathingControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry,
    const SceneObjectMetadata& selection, const SceneObjectDocument& object, bool blocked)
{
    const ProbeVolumeComponent& volume = *object.probeVolume;
    ImGui::SeparatorText("Pathing");

    bool enabled = volume.pathingEnabled;
    ImGui::BeginDisabled(blocked);

    if(ImGui::Checkbox("Enable Pathing", &enabled))
    {
        ProbeVolumeComponent after = volume;
        after.pathingEnabled = enabled;

        if(CommitObjectProbeVolume(scene, selection, object.probeVolume, after))
        {
            m_pathingStatus.erase(object.id);
        }
    }

    ImGui::EndDisabled();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bakes paths between this volume's probes, so sound blocked by geometry can still arrive around it.\n"
            "Audio Sources opt in with their own Pathing toggle.");
    }

    if(!volume.pathingEnabled)
    {
        ImGui::TextDisabled("Status: Disabled%s", volume.bakedPathing.empty() ? "" : " (the baked pathing is kept but not used)");
        return;
    }

    const bool current = m_pathingBake && m_pathingBake->objectID == object.id;
    const PathingStatusEntry& entry = RefreshPathingStatus(scene, object, paths, registry);

    if(current)
    {
        ImGui::Text("Status: Baking");
    }
    else
    {
        switch(entry.status.state)
        {
        case PathingBakes::State::Valid:
            ImGui::TextColored(ValidColor, "Status: %s", ToString(entry.status.state));
            break;
        case PathingBakes::State::Stale:
            ImGui::TextColored(WarningColor, "Status: %s", ToString(entry.status.state));
            break;
        case PathingBakes::State::Missing:
        case PathingBakes::State::Corrupt:
            ImGui::TextColored(ErrorColor, "Status: %s", ToString(entry.status.state));
            break;
        default:
            ImGui::TextDisabled("Status: %s", ToString(entry.status.state));
            break;
        }

        if(!entry.status.message.empty() && entry.status.state != PathingBakes::State::Valid)
        {
            ImGui::TextWrapped("%s", entry.status.message.c_str());
        }

        if(entry.status.state != PathingBakes::State::Valid && !volume.bakedPathing.empty())
        {
            ImGui::TextDisabled("Play does not use this volume's pathing until it is baked again.");
        }
    }

    ImGui::TextDisabled("Asset: %s", volume.bakedPathing.empty() ? "none" : volume.bakedPathing.c_str());
    DrawPathingGuidance(entry.probes, volume.pathingSettings);

    if(current)
    {
        const PathingBakeRun& run = *m_pathingBake;

        if(run.job)
        {
            const std::uint64_t total = run.job->GetTotal();
            const bool baking = run.job->GetStage() == Audio::PathingBakeStage::Baking;
            ImGui::ProgressBar(baking && total > 0 ? static_cast<float>(static_cast<double>((std::min)(run.job->GetDone(), total)) /
                static_cast<double>(total)) : 0.0f, ImVec2(-1.0f, 0.0f), baking ? "Baking" : "Preparing");
        }

        if(!run.stopReason.empty())
        {
            ImGui::TextWrapped("%s: waiting for Steam Audio's bake call to return. It cannot be interrupted; the result will be discarded.",
                run.stopReason.c_str());
        }
        else if(ImGui::Button("Cancel Bake"))
        {
            m_pathingBake->stopReason = "Cancelled";
        }

        ImGui::TextDisabled("Cancelling, or starting Play, waits until the current bake call finishes.");
    }

    const bool busy = m_pathingBake.has_value() || m_reflectionBake.has_value() || m_probeGeneration.has_value() ||
        m_probeBatchRequest.has_value() || m_probePublishTask.IsValid();
    const auto notReady = busy ? std::nullopt : AcousticGeometryNotReady(scene);
    const bool canBake = m_audioSystem && m_audioSystem->GetSteamAudioStatus() == Audio::SteamAudioStatus::Active && !notReady;

    if(notReady)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
        ImGui::TextWrapped("%s", notReady->c_str());
        ImGui::PopStyleColor();
    }

    ImGui::BeginDisabled(blocked || !canBake || busy || !volume.enabled || entry.probes == 0);

    if(ImGui::Button("Bake Pathing"))
    {
        StartPathingBake(scene, object, paths, registry);
    }

    ImGui::EndDisabled();

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Bakes this volume's current probes. Cannot be interrupted once Steam Audio starts the bake.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(blocked || busy || volume.bakedPathing.empty());

    if(ImGui::Button("Clear Pathing Bake"))
    {
        // Only the reference goes, so undo restores the bake. The file waits for Unused Probe Files.
        ProbeVolumeComponent cleared = volume;
        cleared.bakedPathing.clear();

        if(CommitObjectProbeVolume(scene, selection, object.probeVolume, cleared))
        {
            m_pathingMessage = "Pathing bake cleared. Undo restores it.";
            m_pathingStatus.erase(object.id);
        }
    }

    ImGui::EndDisabled();

    if(ImGui::TreeNode("Pathing Bake Settings"))
    {
        PathingBakeSettings settings = volume.pathingSettings;
        bool commit = false;
        ImGui::BeginDisabled(blocked);
        const std::uint32_t minimumSamples = PathingBakeSettings::MinimumSamples;
        const std::uint32_t maximumSamples = PathingBakeSettings::MaximumSamples;
        ImGui::DragScalar("Samples", ImGuiDataType_U32, &settings.samples, 0.25f, &minimumSamples, &maximumSamples, "%u", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragFloat("Visibility Radius (m)", &settings.radius, 0.01f, PathingBakeSettings::MinimumRadius, PathingBakeSettings::MaximumRadius,
            "%.2f", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragFloat("Visibility Threshold", &settings.threshold, 0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragFloat("Visibility Range (m)", &settings.visibilityRange, 0.5f, 0.0f, PathingBakeSettings::MaximumRange, "%.1f",
            ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
        ImGui::DragFloat("Path Range (m)", &settings.pathRange, 0.5f, 0.0f, PathingBakeSettings::MaximumRange, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        commit = ImGui::IsItemDeactivatedAfterEdit() || commit;

        if(ImGui::Button("Reset to Defaults"))
        {
            settings = PathingBakeSettings{};
            commit = true;
        }

        ImGui::EndDisabled();
        ImGui::TextDisabled("Changing a setting makes the baked pathing stale. Materials never do: pathing ignores them.");

        if(commit && !(settings == volume.pathingSettings))
        {
            std::string error;

            if(!ValidatePathingBakeSettings(settings, &error))
            {
                m_probeEditStatus = "Pathing settings refused: " + error + ".";
            }
            else
            {
                ProbeVolumeComponent after = volume;
                after.pathingSettings = settings;

                if(CommitObjectProbeVolume(scene, selection, object.probeVolume, after))
                {
                    m_pathingStatus.erase(object.id);
                }
            }
        }

        ImGui::TreePop();
    }

    if(ImGui::TreeNode("Several Pathing Volumes"))
    {
        ImGui::TextWrapped("Pathing never crosses between volumes. A source is pathed only while it and the listener are both inside the "
            "probes of the same volume. Where volumes overlap, the one a source is already using is kept while it still covers both; "
            "otherwise the first in scene order is chosen. To path between two areas, cover both with one volume.");
        ImGui::TreePop();
    }

    ImGui::Checkbox("Show Pathing in Viewport", &m_pathingOverlay);

    if(!m_pathingMessage.empty())
    {
        ImGui::TextWrapped("%s", m_pathingMessage.c_str());
    }
}

// The Audio Source's side. Everything shown in Play comes from the simulation's own decisions (AudioSystem::GetEventPathingStatus); nothing
// here decides coverage or selection again.
void Editor::DrawAudioSourcePathingStatus(const IDemoScene& scene, const SceneObjectDocument& object)
{
    const auto warn = [](const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };

    if(m_gameState == GameState::Editing)
    {
        const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
        std::size_t baked = 0;

        if(owner && owner->m_runtime && owner->m_runtime->GetFileScene().HasDocument())
        {
            for(const SceneObjectDocument& entry : owner->m_runtime->GetFileScene().GetDocument().objects)
            {
                baked += entry.probeVolume && entry.probeVolume->enabled && entry.probeVolume->pathingEnabled &&
                    !entry.probeVolume->bakedPathing.empty() ? 1 : 0;
            }
        }

        if(baked == 0)
        {
            warn("No Probe Volume has baked pathing: this source is not pathed. Enable Pathing on a Probe Volume and bake it.");
        }

        ImGui::TextDisabled("%s", "Pathed only while this source and the listener are inside the probes of the same baked volume, and its "
            "FMOD event's Steam Audio Spatializer has Apply Occlusion set to Simulation-Defined. Apply Pathing need not be authored. "
            "Checked during Play.");
        return;
    }

    const std::optional<Audio::AudioSystem::PathingEventStatus> status = m_pathingStatusProvider ? m_pathingStatusProvider(object.id) : std::nullopt;

    if(!status)
    {
        warn("Not pathed: the source is not playing, or its event has no Steam Audio Spatializer.");
        return;
    }

    if(!status->available)
    {
        warn("Not pathed: this session runs no pathing simulator (Steam Audio reflections are off).");
        return;
    }

    if(!status->occlusionApplied)
    {
        warn("Incompatible FMOD event: its Steam Audio Spatializer does not apply simulated occlusion (Apply Occlusion is not "
            "Simulation-Defined), so the direct sound is never reduced and pathing has nothing to restore. Engine does not change "
            "authored settings; change it in FMOD Studio.");
    }

    ImGui::TextDisabled("Spatializer: occlusion %s, transmission %s, Apply Pathing authored %s (Engine controls it).",
        status->occlusionApplied ? "simulated" : "not simulated", status->transmissionApplied ? "simulated" : "not simulated",
        status->authoredApplyPathing ? "on" : "off");

    const std::vector<Audio::AudioSystem::PathingVolumeDisplay> volumes = m_audioSystem ? m_audioSystem->GetPathingVolumesForDisplay() :
        std::vector<Audio::AudioSystem::PathingVolumeDisplay>{};

    if(volumes.empty())
    {
        warn("Not pathed: no valid baked pathing volume was registered for this Play.");
        return;
    }

    if(status->capacity == Audio::AudioSystem::PathingEventStatus::Capacity::Waiting)
    {
        warn("Waiting: the pathing source limit is reached. It keeps its usual sound until a pathed source stops.");
        return;
    }

    if(!status->sourceCovered)
    {
        warn("Not pathed: the source is outside every pathing volume's probes.");
    }
    else if(!status->listenerCovered)
    {
        warn("Not pathed: the listener is outside every pathing volume's probes.");
    }
    else if(status->batch < 0)
    {
        warn("Not pathed: the source and the listener are in different pathing volumes. Pathing never crosses between volumes.");
    }
    else
    {
        const std::size_t index = static_cast<std::size_t>(status->batch);
        const std::string name = index < volumes.size() ? volumes[index].name : std::to_string(status->batch);
        ImGui::TextColored(ValidColor, "Pathed through %s: %.0f%% of the blocked sound restored%s.", name.c_str(),
            static_cast<double>(status->gain) * 100.0, status->applyPathing ? "" : " (Apply Pathing not yet on)");
    }
}
