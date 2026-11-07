#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Engine/ProbeVolumes.h"
#include "Scene/FileSceneRuntime.h"
#include "assets/ProjectAssetResolver.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace
{
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);
    const ImVec4 ValidColor(0.45f, 0.85f, 0.45f, 1.0f);
    constexpr double FingerprintRefreshSeconds = 0.5;

    Audio::AcousticTransform ToTransform(const Matrix4& world)
    {
        Audio::AcousticTransform transform{};

        for(std::size_t row = 0; row < 4; ++row)
        {
            for(std::size_t column = 0; column < 4; ++column)
            {
                transform[row][column] = world.m[row][column];
            }
        }

        return transform;
    }

    ProbeVolumes::SceneAccess MakeSceneAccess(const FileSceneRuntime& runtime, const IDemoScene& scene)
    {
        ProbeVolumes::SceneAccess access;
        access.world = [&runtime](const SceneObjectDocument& object) -> std::optional<Audio::AcousticTransform>
        {
            const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
            const auto& objects = runtime.GetScene().GetRenderObjects();
            const auto found = mapping.find(object.id);

            if(found == mapping.end() || found->second >= objects.size())
            {
                return std::nullopt;
            }

            return ToTransform(objects[found->second].GetPresentationTransform().currentModel);
        };
        access.mesh = [&runtime, &scene](const SceneObjectDocument& object) -> const ImportedMesh*
        {
            const auto& mapping = runtime.GetObjectMapping().idToRuntimeIndex;
            const auto& objects = runtime.GetScene().GetRenderObjects();
            const auto found = mapping.find(object.id);

            if(found == mapping.end() || found->second >= objects.size() || !objects[found->second].mesh)
            {
                return nullptr;
            }

            return scene.GetMeshGeometry(*objects[found->second].mesh);
        };
        return access;
    }
}

// One complete replacement of the object's Probe Volume per history entry.
bool Editor::CommitObjectProbeVolume(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<ProbeVolumeComponent>& before, const std::optional<ProbeVolumeComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_probeEditStatus = "Probe Volumes can be edited in a stopped scene without pending edits.";
        return false;
    }

    if(after && !ValidateProbeVolume(*after, &m_probeEditStatus))
    {
        return false;
    }

    if(before == after)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<ProbeVolumeEdit>(ProbeVolumeEdit{*m_historySession, selection.objectID, before, after});
        const std::size_t manualBytes = (before ? before->manualProbes.capacity() : 0) + (after ? after->manualProbes.capacity() : 0);
        const std::size_t bytes = sizeof(SceneHistoryEntry) + sizeof(ProbeVolumeEdit) + command->objectID.capacity() +
            manualBytes * sizeof(std::array<float, 3>);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_probeEditStatus = "This edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectProbeVolume(selection, before, after, &m_probeEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_probeEditStatus.clear();
        m_probeView.nextFingerprint = 0.0;
        return true;
    }
    catch(const std::exception& exception)
    {
        m_probeEditStatus = exception.what();
        return false;
    }
}

void Editor::RefreshProbeView(const FileBackedDemoScene& owner, const IDemoScene& scene, const SceneObjectDocument& object,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    ProbeView& view = m_probeView;
    const ProbeVolumeComponent& volume = *object.probeVolume;
    const auto id = AssetID::ParseReference(volume.probeBatch);
    std::filesystem::file_time_type fileTime{};
    bool fileExists = false;

    if(id)
    {
        const AssetResolution resolution = ProjectAssetResolver::Resolve(paths, registry, *id);

        if(resolution.status == AssetAvailability::Available)
        {
            std::error_code error;
            fileTime = std::filesystem::last_write_time(resolution.resolvedPath, error);
            fileExists = !error;
        }
    }

    // Reading the file on each frame would stall the Inspector, so only a changed reference or modification time reads it.
    if(view.objectID != object.id || view.reference != volume.probeBatch || view.fileExists != fileExists || view.fileTime != fileTime)
    {
        view.objectID = object.id;
        view.reference = volume.probeBatch;
        view.fileExists = fileExists;
        view.fileTime = fileTime;
        view.stored.reset();
        view.loadError.clear();
        view.nextFingerprint = 0.0;

        if(fileExists)
        {
            ProbeBatchAssets::Data data;

            if(ProbeBatchAssets::LoadFromFile(ProjectAssetResolver::Resolve(paths, registry, *id).resolvedPath, data, &view.loadError))
            {
                view.stored = std::move(data);
            }
        }
    }

    // Stale detection hashes the scene's vertices, so it is throttled rather than run per frame.
    const double now = ImGui::GetTime();

    if(now >= view.nextFingerprint)
    {
        view.fingerprint = ProbeVolumes::Fingerprint(owner.m_runtime->GetFileScene().GetDocument(), object, MakeSceneAccess(*owner.m_runtime, scene));
        view.nextFingerprint = now + FingerprintRefreshSeconds;
    }

    if(volume.probeBatch.empty())
    {
        view.status = ProbeVolumes::Status::NotGenerated;
    }
    else if(!fileExists)
    {
        view.status = ProbeVolumes::Status::Missing;
    }
    else if(!view.stored)
    {
        view.status = ProbeVolumes::Status::Invalid;
    }
    else
    {
        view.status = view.stored->fingerprint == view.fingerprint ? ProbeVolumes::Status::Valid : ProbeVolumes::Status::Stale;
    }
}

void Editor::GenerateProbes(IDemoScene& scene, const FileBackedDemoScene& owner, const SceneObjectDocument& object)
{
    if(!m_audioSystem || m_probeGeneration || !m_historySession)
    {
        m_probeEditStatus = m_audioSystem ? "Another generation is running." : "Audio is not running.";
        return;
    }

    const SceneDocument& document = owner.m_runtime->GetFileScene().GetDocument();
    const ProbeVolumes::SceneAccess access = MakeSceneAccess(*owner.m_runtime, scene);
    ProbeGeneration generation;
    generation.objectID = object.id;
    generation.session = *m_historySession;
    std::string error;
    auto request = ProbeVolumes::BuildRequest(document, object, access, generation.skippedObjects, error);

    if(!request)
    {
        // Existing probes stay: failing to regenerate must not lose them.
        m_probeEditStatus = "Generation failed: " + error + ".";
        return;
    }

    generation.fingerprint = ProbeVolumes::Fingerprint(document, object, access);
    generation.job = m_audioSystem->StartProbeGeneration(std::move(*request));

    if(!generation.job)
    {
        m_probeEditStatus = "Generation failed: Steam Audio is not active, or another generation is still stopping.";
        return;
    }

    m_probeEditStatus.clear();
    m_probeGeneration = std::move(generation);
}

void Editor::PollProbeGeneration(IDemoScene& scene)
{
    if(!m_probeGeneration)
    {
        return;
    }

    ProbeGeneration& generation = *m_probeGeneration;
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    // Play and scene changes own the scene from here: the result could not be applied to it.
    const bool sceneLost = m_gameState != GameState::Editing || !owner || !RenameSessionMatches(scene, generation.session);

    if(sceneLost)
    {
        generation.job->Cancel();
    }

    if(!generation.job->IsFinished())
    {
        return;
    }

    const ProbeGeneration finished = std::move(generation);
    m_probeGeneration.reset();
    Audio::ProbeGenerationResult& result = finished.job->GetResult();

    // A job that finished just before the scene was lost is cancelled too: nothing may be stored for a scene that can't accept it.
    if(result.cancelled || sceneLost)
    {
        m_probeEditStatus = "Generation cancelled. Existing probes are unchanged.";
        return;
    }

    if(!result.success)
    {
        m_probeEditStatus = "Generation failed: " + result.error + ". Existing probes are unchanged.";
        return;
    }

    SceneObjectDocument object;
    const SceneDocument& document = owner->m_runtime->GetFileScene().GetDocument();

    if(!owner->CaptureTriggerColliderInspection(finished.objectID, object) || !object.probeVolume)
    {
        m_probeEditStatus = "The Probe Volume's object is gone. Nothing was stored.";
        return;
    }

    // Everything the probes were generated from is in the fingerprint, so this catches any edit made in the meantime.
    if(ProbeVolumes::Fingerprint(document, object, MakeSceneAccess(*owner->m_runtime, scene)) != finished.fingerprint)
    {
        m_probeEditStatus = "The scene changed during generation, so the result was discarded. Existing probes are unchanged.";
        return;
    }

    const ProbeVolumes::GenerationOutcome outcome = ProbeVolumes::FinishGeneration(std::move(result), finished.fingerprint, finished.skippedObjects);
    m_probeBatchRequest = ProbeBatchRequest{finished.objectID, ProbeBatchAssets::Encode(outcome.data)};
    m_probeEditStatus = "Generated " + std::to_string(outcome.data.probes.size()) + (outcome.data.probes.size() == 1 ? " probe." : " probes.");

    if(outcome.duplicatesRemoved > 0)
    {
        m_probeEditStatus += " " + std::to_string(outcome.duplicatesRemoved) + " duplicate manual probes were dropped.";
    }

    if(!outcome.skippedObjects.empty())
    {
        m_probeEditStatus += " " + std::to_string(outcome.skippedObjects.size()) + " acoustic objects had no usable geometry (first: " +
            outcome.skippedObjects.front() + ").";
    }

    if(outcome.enclosedByOneShell)
    {
        m_probeEditStatus += " The whole volume is inside closed geometry, which looks like a room with outward faces, so all probes were kept.";
    }

    if(outcome.openPieces > 0)
    {
        m_probeEditStatus += " " + std::to_string(outcome.openPieces) + " surfaces aren't closed, so the filter can't tell what is inside them.";
    }
}

void Editor::DrawProbeGenerationProgress()
{
    const Audio::ProbeGenerationJob& job = *m_probeGeneration->job;
    const std::uint64_t total = job.GetTotal();
    const char* stage = "Starting";
    bool interruptible = true;

    switch(job.GetStage())
    {
        case Audio::ProbeGenerationStage::BuildingScene:
            stage = "Building the generation scene";
            break;
        case Audio::ProbeGenerationStage::Placing:
            stage = "Placing probes";
            interruptible = false;
            break;
        case Audio::ProbeGenerationStage::Filtering:
            stage = "Filtering probes inside solids";
            break;
        case Audio::ProbeGenerationStage::Packing:
            stage = "Packing the batch";
            break;
        default:
            break;
    }

    if(total > 0)
    {
        ImGui::ProgressBar(static_cast<float>(static_cast<double>(job.GetDone()) / static_cast<double>(total)), ImVec2(-1.0f, 0.0f), stage);
    }
    else
    {
        ImGui::TextUnformatted(stage);
    }

    if(job.IsCancelRequested())
    {
        ImGui::TextDisabled(interruptible ? "Cancelling..." : "Cancelling. Steam Audio's placement can't be interrupted, so this finishes when it does.");
    }
    else if(ImGui::Button("Cancel Generation"))
    {
        m_probeGeneration->job->Cancel();
    }
}

std::set<std::string> Editor::CollectLiveProbeReferences(const FileBackedDemoScene& owner) const
{
    std::set<std::string> references;
    const auto add = [&](const std::optional<ProbeVolumeComponent>& volume)
    {
        if(volume && !volume->probeBatch.empty())
        {
            references.insert(volume->probeBatch);
        }
    };

    for(const SceneObjectDocument& object : owner.m_runtime->GetFileScene().GetDocument().objects)
    {
        add(object.probeVolume);
    }

    // Deleted objects and past component states alike: undo and redo can bring either back.
    for(const SceneHistoryEntry& entry : m_sceneHistory)
    {
        if(const auto* edit = std::get_if<std::unique_ptr<ProbeVolumeEdit>>(&entry.command); edit && *edit)
        {
            add((*edit)->before);
            add((*edit)->after);
        }
        else if(const auto* deletion = std::get_if<std::unique_ptr<ObjectDeletion>>(&entry.command); deletion && *deletion)
        {
            add((*deletion)->object.probeVolume);
        }
    }

    return references;
}

void Editor::OnProbeCleanupCompleted(std::size_t removed, const std::string& error)
{
    m_probeCleanupReport.reset();
    m_probeEditStatus = "Deleted " + std::to_string(removed) + " unused probe files.";

    if(!error.empty())
    {
        m_probeEditStatus += " Stopped: " + error + ".";
    }
}

void Editor::DrawProbeCleanup(const FileBackedDemoScene& owner, const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool blocked)
{
    if(!ImGui::TreeNode("Unused Probe Files"))
    {
        return;
    }

    ImGui::TextDisabled("Clear and Regenerate keep old files, so Undo and other scenes stay valid. A file counts as unused only when no scene in the project, the open document or the undo history references it.");
    ImGui::BeginDisabled(blocked || m_gameState != GameState::Editing || m_probeGeneration.has_value());

    if(ImGui::Button("Find Unused Files"))
    {
        m_probeCleanupReport = ProbeAssetCleanup::Scan(paths, registry, CollectLiveProbeReferences(owner));
    }

    if(m_probeCleanupReport)
    {
        const ProbeAssetCleanup::Report& report = *m_probeCleanupReport;

        if(!report.conclusive)
        {
            ImGui::TextColored(WarningColor, "Nothing can be deleted: %s.", report.reason.c_str());
        }
        else
        {
            ImGui::Text("%zu unused (%.1f MB), %zu in use, across %zu saved scenes.", report.unused.size(), static_cast<double>(report.unusedBytes) / 1048576.0, report.referenced, report.scenesRead);

            if(!report.unregisteredFiles.empty())
            {
                ImGui::TextWrapped("%zu probe files have no registration and are left alone (first: %s).", report.unregisteredFiles.size(), report.unregisteredFiles.front().generic_string().c_str());
            }

            ImGui::BeginDisabled(report.unused.empty());

            if(ImGui::Button("Delete Unused Files..."))
            {
                ImGui::OpenPopup("Delete unused probe files?");
            }

            ImGui::EndDisabled();
        }
    }

    if(ImGui::BeginPopupModal("Delete unused probe files?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("This permanently deletes %zu probe files. Regenerating can recreate them.", m_probeCleanupReport ? m_probeCleanupReport->unused.size() : std::size_t{0});

        if(ImGui::Button("Delete") && m_probeCleanupReport)
        {
            m_probeCleanupRequest = ProbeCleanupRequest{CollectLiveProbeReferences(owner), m_probeCleanupReport->unused};
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if(ImGui::Button("Keep Them"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
    ImGui::TreePop();
}

// Engine owns the registry, so it performs the store and hands back the reference.
void Editor::OnProbeBatchRequestCompleted(IDemoScene& scene, const std::string& objectID, const std::string& reference, const std::string& error)
{
    if(!error.empty())
    {
        m_probeEditStatus = "Could not store the probes: " + error + ".";
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;
    std::string lookupError;
    const auto selection = owner ? owner->GetObjectMetadata(objectID, &lookupError) : std::nullopt;

    if(!selection || !owner->CaptureTriggerColliderInspection(objectID, object) || !object.probeVolume)
    {
        m_probeEditStatus = "The Probe Volume's object is gone.";
        return;
    }

    ProbeVolumeComponent after = *object.probeVolume;
    after.probeBatch = reference;
    const std::string status = m_probeEditStatus;

    if(CommitObjectProbeVolume(scene, *selection, object.probeVolume, after))
    {
        m_probeEditStatus = status;
    }

    // Same reference, new contents: the view must reread them.
    m_probeView.objectID.clear();
}

void Editor::DrawObjectProbeVolumeControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    std::optional<SceneObjectMetadata> selection(std::in_place);
    SceneObjectDocument object;

    if(!owner->InspectSelectedObject(m_selectedObject, *selection, object) || !object.probeVolume)
    {
        return;
    }

    const std::optional<ProbeVolumeComponent> before = object.probeVolume;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection) || IsSceneEditingLocked();
    RefreshProbeView(*owner, scene, object, paths, registry);

    // The draft holds a drag in progress, until the document itself changes.
    if(m_probeDraftObject != object.id || m_probeDraftBase != *before)
    {
        m_probeDraft = *before;
        m_probeDraftBase = *before;
        m_probeDraftObject = object.id;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("ProbeVolume");
    bool open = true;
    const bool expanded = ImGui::CollapsingHeader("Steam Audio Probe Volume", &open, ImGuiTreeNodeFlags_DefaultOpen);

    if(!open && !blocked)
    {
        // The probes' asset stays, so undo brings the volume back whole; Clear Probes is what deletes data.
        CommitObjectProbeVolume(scene, *selection, before, std::nullopt);
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    if(!expanded)
    {
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    ImGui::BeginDisabled(blocked);
    ProbeVolumeComponent& draft = m_probeDraft;
    bool commit = false;

    if(ImGui::Checkbox("Enabled", &draft.enabled))
    {
        commit = true;
    }

    static constexpr const char* Placements[] = {"Floor (Steam Audio)", "Centroid (Steam Audio)", "Uniform 3D (Engine grid)"};
    int placement = static_cast<int>(draft.placement);

    if(ImGui::Combo("Placement", &placement, Placements, IM_ARRAYSIZE(Placements)))
    {
        draft.placement = static_cast<ProbePlacement>(placement);
        commit = true;
    }

    ImGui::DragFloat3("Volume Size", draft.size.data(), 0.05f, 0.01f, 1000000.0f, "%.2f");
    commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
    ImGui::BeginDisabled(draft.placement == ProbePlacement::Centroid);
    ImGui::DragFloat("Probe Spacing (m)", &draft.probeSpacing, 0.02f, 0.05f, 1000.0f, "%.2f");
    commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(draft.placement != ProbePlacement::UniformFloor);
    ImGui::DragFloat("Height Above Floor (m)", &draft.heightAboveFloor, 0.02f, 0.0f, 1000.0f, "%.2f");
    commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
    ImGui::EndDisabled();

    switch(draft.placement)
    {
        case ProbePlacement::UniformFloor:
            ImGui::TextDisabled("Rays run down from the top of the box. Each surface found gets a probe at this height, and stacked floors each get their own. The search continues to the floor beneath solid objects, so they can hold probes unless the filter below removes them.");
            break;
        case ProbePlacement::Centroid:
            ImGui::TextDisabled("One probe at the object's origin, with a radius of half the box's smallest side.");
            break;
        case ProbePlacement::Uniform3D:
            ImGui::TextWrapped("Steam Audio has no 3D mode: Engine computes this grid. It ignores geometry, so probes can land inside solid objects unless the filter below removes them.");
            break;
    }

    ImGui::BeginDisabled(draft.placement == ProbePlacement::Centroid);

    if(ImGui::Checkbox("Filter Probes Inside Solids", &draft.filterInsideSolids))
    {
        commit = true;
    }

    ImGui::EndDisabled();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Removes generated probes inside fully closed acoustic meshes. Open or uncertain geometry keeps its probes and is counted. Manual probes are never removed.");
    }

    ProbeVolumes::GenerationEstimate estimate;

    if(const auto world = MakeSceneAccess(*owner->m_runtime, scene).world(object))
    {
        estimate = ProbeVolumes::Estimate(draft, *world);
        ImGui::Text(estimate.exact ? "Will generate %llu probes." : "Will test about %llu probe positions; the result depends on the geometry.", static_cast<unsigned long long>(estimate.probes));

        if(estimate.needsConfirmation)
        {
            ImGui::TextColored(WarningColor, "This is a large generation. It runs in the background, but may take a long time and a lot of memory.");
        }

        if(!estimate.fitsAsset)
        {
            ImGui::TextColored(WarningColor, "This is likely more than one probe file can hold (%zu MB). Increase the spacing or split the volume.", ProbeBatchAssets::MaximumBytes / 1048576);
        }
    }

    if(ImGui::TreeNode("Manual Probes"))
    {
        std::optional<std::size_t> removed;

        for(std::size_t index = 0; index < draft.manualProbes.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            ImGui::DragFloat3("##Position", draft.manualProbes[index].data(), 0.05f);
            commit = ImGui::IsItemDeactivatedAfterEdit() || commit;
            ImGui::SameLine();

            if(ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
            {
                removed = index;
            }

            ImGui::PopID();
        }

        if(removed)
        {
            draft.manualProbes.erase(draft.manualProbes.begin() + static_cast<std::ptrdiff_t>(*removed));
            commit = true;
        }

        if(ImGui::Button("Add Manual Probe"))
        {
            draft.manualProbes.push_back({0.0f, 0.0f, 0.0f});
            commit = true;
        }

        ImGui::TextDisabled("Positions are in the object's local space and are added to the placement's probes.");
        ImGui::TreePop();
    }

    if(commit && !(draft == *before))
    {
        const ProbeVolumeComponent accepted = draft;

        if(!CommitObjectProbeVolume(scene, *selection, before, accepted))
        {
            m_probeDraft = *before;
        }
    }

    ImGui::Separator();
    const bool canGenerate = m_audioSystem && m_audioSystem->GetSteamAudioStatus() == Audio::SteamAudioStatus::Active;
    const bool generating = m_probeGeneration.has_value();
    ImGui::BeginDisabled(!canGenerate || generating || m_probeBatchRequest.has_value() || (estimate.exact && !estimate.fitsAsset));

    if(ImGui::Button(before->probeBatch.empty() ? "Generate Probes" : "Regenerate Probes"))
    {
        if(estimate.needsConfirmation)
        {
            ImGui::OpenPopup("Generate this many probes?");
        }
        else
        {
            GenerateProbes(scene, *owner, object);
        }
    }

    if(ImGui::BeginPopupModal("Generate this many probes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("About %llu probe positions. You can cancel while it runs.", static_cast<unsigned long long>(estimate.probes));

        if(ImGui::Button("Generate"))
        {
            GenerateProbes(scene, *owner, object);
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if(ImGui::Button("Not Now"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(before->probeBatch.empty() || generating || m_probeBatchRequest.has_value());

    if(ImGui::Button("Clear Probes"))
    {
        // Only the reference goes, so undo restores working probes. The file waits for Unused Probe Files.
        ProbeVolumeComponent cleared = *before;
        cleared.probeBatch.clear();

        if(CommitObjectProbeVolume(scene, *selection, before, cleared))
        {
            m_probeEditStatus = "Probes cleared. Undo restores them.";
        }
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if(m_probeGeneration)
    {
        DrawProbeGenerationProgress();
    }

    if(!canGenerate)
    {
        ImGui::TextDisabled("Generation needs Steam Audio active in this project.");
    }

    const ProbeVolumes::Status status = m_probeView.status;
    const bool good = status == ProbeVolumes::Status::Valid;
    const bool quiet = status == ProbeVolumes::Status::NotGenerated;
    const std::size_t stored = m_probeView.stored ? m_probeView.stored->probes.size() : 0;

    if(quiet)
    {
        ImGui::TextDisabled("Status: %s", ProbeVolumes::ToString(status));
    }
    else
    {
        ImGui::TextColored(good ? ValidColor : WarningColor, "Status: %s, %zu probes", ProbeVolumes::ToString(status), stored);
    }

    if(m_probeView.stored && (m_probeView.stored->rejectedProbes > 0 || m_probeView.stored->ambiguousProbes > 0))
    {
        ImGui::Text("Filter: %u removed inside solids, %u uncertain and kept.", m_probeView.stored->rejectedProbes, m_probeView.stored->ambiguousProbes);
    }

    if(status == ProbeVolumes::Status::Stale)
    {
        ImGui::TextWrapped("The settings or scene changed since generation. Regenerate: Play ignores these probes.");
    }
    else if(status == ProbeVolumes::Status::Missing)
    {
        ImGui::TextWrapped("The probe file is gone. Regenerate to recreate it.");
    }
    else if(status == ProbeVolumes::Status::Invalid)
    {
        ImGui::TextWrapped("The probe file failed validation: %s. Regenerate to replace it.", m_probeView.loadError.c_str());
    }

    if(m_gameState != GameState::Editing)
    {
        ImGui::TextDisabled("Probe changes apply the next time Play starts.");
    }

    if(!m_probeEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_probeEditStatus.c_str());
    }

    DrawProbeCleanup(*owner, paths, registry, blocked);

    // What the viewport draws this frame.
    m_probeOverlay.frame = ImGui::GetFrameCount();
    m_probeOverlay.valid = good;
    m_probeOverlay.size = draft.size;
    m_probeOverlay.objectID = object.id;

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

// Wire box and probe dots of the selected volume.
void Editor::DrawProbeVolumeOverlay(const IDemoScene& scene, ImDrawList& draw, const std::function<std::optional<ImVec2>(const Vector3&)>& project) const
{
    if(ImGui::GetFrameCount() - m_probeOverlay.frame > 1 || m_probeOverlay.objectID.empty())
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);

    if(!owner || !owner->m_runtime)
    {
        return;
    }

    const auto& mapping = owner->m_runtime->GetObjectMapping().idToRuntimeIndex;
    const auto& objects = owner->m_runtime->GetScene().GetRenderObjects();
    const auto found = mapping.find(m_probeOverlay.objectID);

    if(found == mapping.end() || found->second >= objects.size())
    {
        return;
    }

    const Matrix4 model = objects[found->second].GetPresentationTransform().currentModel;
    const ImU32 boxColor = m_probeOverlay.valid ? IM_COL32(110, 230, 110, 230) : IM_COL32(255, 170, 70, 230);
    const ImU32 probeColor = m_probeOverlay.valid ? IM_COL32(90, 210, 255, 230) : IM_COL32(255, 170, 70, 180);
    std::array<std::optional<ImVec2>, 8> corners;

    for(std::size_t index = 0; index < corners.size(); ++index)
    {
        const float x = ((index & 1) ? 0.5f : -0.5f) * m_probeOverlay.size[0];
        const float y = ((index & 2) ? 0.5f : -0.5f) * m_probeOverlay.size[1];
        const float z = ((index & 4) ? 0.5f : -0.5f) * m_probeOverlay.size[2];
        const Vector3 world{model.m[0][0] * x + model.m[0][1] * y + model.m[0][2] * z + model.m[0][3],
            model.m[1][0] * x + model.m[1][1] * y + model.m[1][2] * z + model.m[1][3],
            model.m[2][0] * x + model.m[2][1] * y + model.m[2][2] * z + model.m[2][3]};
        corners[index] = project(world);
    }

    for(std::size_t index = 0; index < corners.size(); ++index)
    {
        for(const std::size_t bit : {std::size_t{1}, std::size_t{2}, std::size_t{4}})
        {
            if(!(index & bit) && corners[index] && corners[index | bit])
            {
                draw.AddLine(*corners[index], *corners[index | bit], boxColor, 1.5f);
            }
        }
    }

    if(m_probeView.objectID != m_probeOverlay.objectID || !m_probeView.stored)
    {
        return;
    }

    for(const std::array<float, 4>& probe : m_probeView.stored->probes)
    {
        if(const auto point = project(Vector3{probe[0], probe[1], probe[2]}))
        {
            draw.AddCircleFilled(*point, 3.0f, probeColor, 8);
        }
    }
}
