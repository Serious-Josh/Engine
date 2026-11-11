// VX.4C.1: voxel baking in the Editor: per-object participation and orientation (Inspector), the Voxels window (settings, bake,
// progress, result, current status, participation), and their scene history. See Documentation/Voxel/VoxelArchitecture.md, 6.8.
//
// A bake captures an immutable snapshot on the main thread, so later edits cannot reach it; it bakes on a worker, and Engine's
// per-frame PollVoxelBake publishes it on the main thread, only while edits are allowed, and only if the scene and its voxel
// settings are still exactly what was captured. Publication then records the manifest reference as one history entry, so it is
// dirty, undoable and saved like any edit; cancelling or failing never touches the scene's manifest.

#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Editor/VoxelSceneAccess.h"
#include "Engine/VoxelProjectStore.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace
{
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);
    const ImVec4 ErrorColor(1.0f, 0.4f, 0.4f, 1.0f);
    const ImVec4 GoodColor(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 DimColor(0.6f, 0.6f, 0.6f, 1.0f);

    constexpr VoxelParticipation Participations[] = {VoxelParticipation::Automatic, VoxelParticipation::Static, VoxelParticipation::Excluded};
    constexpr const char* ParticipationLabels[] = {"Automatic", "Static", "Excluded"};
    constexpr VoxelOrientation Orientations[] = {VoxelOrientation::Automatic, VoxelOrientation::AsAuthored, VoxelOrientation::Outward};
    constexpr const char* OrientationLabels[] = {"Automatic", "As Authored", "Outward"};

    std::string HashText(std::uint64_t hash)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
        return text;
    }

    std::string MetresText(int exponent)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%g m", std::ldexp(1.0, exponent));
        return text;
    }

    // An absent component is Automatic/Automatic; the scene keeps no component for it.
    std::optional<VoxelSettings> Normalised(const VoxelSettings& settings)
    {
        return settings == VoxelSettings{} ? std::nullopt : std::optional<VoxelSettings>(settings);
    }

    ImVec4 StateColor(VoxelScene::BakeState state)
    {
        switch(state)
        {
        case VoxelScene::BakeState::UpToDate:
            return GoodColor;
        case VoxelScene::BakeState::Stale:
        case VoxelScene::BakeState::SettingsChanged:
            return WarningColor;
        case VoxelScene::BakeState::Missing:
        case VoxelScene::BakeState::Corrupt:
            return ErrorColor;
        case VoxelScene::BakeState::NotBaked:
            break;
        }
        return DimColor;
    }
}

size_t Editor::VoxelEditPayloadBytes(const SceneHistorySession& session, std::size_t changes)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(VoxelSettingsEdit) + sizeof(VoxelBakeEdit) + session.sceneIdentity.capacity() +
        pathBytes(session.projectIdentity) + pathBytes(session.scenePath) + changes * (sizeof(VoxelObjectChange) + 64);
}

// One history entry for any number of objects' voxel settings.
bool Editor::CommitVoxelSettings(IDemoScene& scene, std::vector<VoxelObjectChange> changes)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_voxelEditStatus = "Voxel settings can be edited in a stopped scene without pending edits.";
        return false;
    }

    changes.erase(std::remove_if(changes.begin(), changes.end(), [](const VoxelObjectChange& change) { return change.before == change.after; }),
        changes.end());

    if(changes.empty())
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<VoxelSettingsEdit>(VoxelSettingsEdit{*m_historySession, changes});
        const size_t bytes = VoxelEditPayloadBytes(*m_historySession, changes.size());

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_voxelEditStatus = "This edit exceeds the scene history payload limit.";
            return false;
        }

        std::vector<FileSceneRuntime::VoxelSettingsChange> apply;
        for(const VoxelObjectChange& change : changes)
        {
            apply.push_back({change.objectID, change.before, change.after});
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectsVoxelSettings(apply, &m_voxelEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_voxelEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_voxelEditStatus = exception.what();
        return false;
    }
}

// One history entry for the scene's voxel bake: a settings edit, or a published bake's manifest.
bool Editor::CommitVoxelBake(IDemoScene& scene, const std::optional<VoxelBakeSettings>& before, const std::optional<VoxelBakeSettings>& after,
    std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "Voxel bake settings can be changed in a stopped scene without pending edits.";
        }
        return false;
    }

    if(before == after)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<VoxelBakeEdit>(VoxelBakeEdit{*m_historySession, before, after});
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, VoxelEditPayloadBytes(*m_historySession, 0)};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneVoxelBake(before, after, error))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_voxelStatusRequested = true;
        return true;
    }
    catch(const std::exception& exception)
    {
        if(error)
        {
            *error = exception.what();
        }
        return false;
    }
}

bool Editor::ReplayVoxelSettings(IDemoScene& scene, const VoxelSettingsEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Voxel settings replay belongs to a stale scene session.";
        return false;
    }

    std::vector<FileSceneRuntime::VoxelSettingsChange> apply;
    for(const VoxelObjectChange& change : edit.changes)
    {
        apply.push_back({change.objectID, redo ? change.before : change.after, redo ? change.after : change.before});
    }

    return owner->SetObjectsVoxelSettings(apply, &error);
}

bool Editor::ReplayVoxelBake(IDemoScene& scene, const VoxelBakeEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Voxel bake replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneVoxelBake(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Inspector

void Editor::DrawObjectVoxelControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    // The selected mesh objects, primary first: a bulk edit applies to all of them.
    struct Target
    {
        SceneObjectMetadata metadata;
        SceneObjectDocument object;
    };
    std::vector<Target> targets;
    std::vector<size_t> order;
    order.push_back(m_selectedObject);
    for(const size_t index : m_selectedObjects)
    {
        if(index != m_selectedObject)
        {
            order.push_back(index);
        }
    }

    for(const size_t index : order)
    {
        Target target;
        if(index != InvalidObjectSelection && owner->InspectSelectedObject(index, target.metadata, target.object) && !target.object.meshReference.empty())
        {
            targets.push_back(std::move(target));
        }
    }

    if(targets.empty())
    {
        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID("Voxels");

    if(!ImGui::CollapsingHeader("Voxels", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    if(targets.size() > 1)
    {
        ImGui::TextDisabled("%zu selected mesh objects", targets.size());
    }

    const bool blocked = IsSceneEditingLocked() || !ResolveComponentEditTarget(scene, targets.front().metadata);
    ImGui::BeginDisabled(blocked);

    const auto commitAll = [&](auto change) {
        std::vector<VoxelObjectChange> changes;
        for(const Target& target : targets)
        {
            VoxelSettings settings = target.object.voxel.value_or(VoxelSettings{});
            change(settings);
            changes.push_back({target.object.id, target.object.voxel, Normalised(settings)});
        }
        CommitVoxelSettings(scene, std::move(changes));
    };

    // Participation: the shared value, or "(mixed)".
    const VoxelParticipation participation = targets.front().object.voxel.value_or(VoxelSettings{}).participation;
    const bool participationMixed = std::any_of(targets.begin(), targets.end(),
        [&](const Target& t) { return t.object.voxel.value_or(VoxelSettings{}).participation != participation; });
    const int participationIndex = static_cast<int>(participation);

    if(ImGui::BeginCombo("Participation", participationMixed ? "(mixed)" : ParticipationLabels[participationIndex]))
    {
        for(int index = 0; index < 3; ++index)
        {
            if(ImGui::Selectable(ParticipationLabels[index], !participationMixed && index == participationIndex))
            {
                commitAll([&](VoxelSettings& settings) { settings.participation = Participations[index]; });
            }
        }
        ImGui::EndCombo();
    }

    const VoxelOrientation orientation = targets.front().object.voxel.value_or(VoxelSettings{}).orientation;
    const bool orientationMixed = std::any_of(targets.begin(), targets.end(),
        [&](const Target& t) { return t.object.voxel.value_or(VoxelSettings{}).orientation != orientation; });
    const int orientationIndex = static_cast<int>(orientation);

    if(ImGui::BeginCombo("Orientation", orientationMixed ? "(mixed)" : OrientationLabels[orientationIndex]))
    {
        for(int index = 0; index < 3; ++index)
        {
            if(ImGui::Selectable(OrientationLabels[index], !orientationMixed && index == orientationIndex))
            {
                commitAll([&](VoxelSettings& settings) { settings.orientation = Orientations[index]; });
            }
        }
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Automatic: the scene policy (an inverted closed mesh outside any solid counts as solid).\n"
                          "As Authored: inverted meshes stay inverted (intentional cavities).\n"
                          "Outward: every closed mesh of the object is solid.");
    }

    // What the policy decides for these objects now.
    if(targets.size() == 1)
    {
        const VoxelScene::ParticipationDecision decision = VoxelScene::ResolveParticipation(targets.front().object);
        ImGui::TextColored(decision.participates ? GoodColor : DimColor, "%s: %s", decision.participates ? "Bakes" : "Not baked",
            VoxelScene::ParticipationReasonToString(decision.reason));
        if(decision.warning)
        {
            ImGui::TextColored(WarningColor, "Marked Static, but a moving body is never baked.");
        }
    }
    else
    {
        std::size_t participating = 0;
        std::size_t warnings = 0;
        for(const Target& target : targets)
        {
            const VoxelScene::ParticipationDecision decision = VoxelScene::ResolveParticipation(target.object);
            participating += decision.participates;
            warnings += decision.warning;
        }
        ImGui::TextColored(participating > 0 ? GoodColor : DimColor, "%zu of %zu bake", participating, targets.size());
        if(warnings > 0)
        {
            ImGui::TextColored(WarningColor, "%zu marked Static on a moving body (not baked).", warnings);
        }
    }

    if(!m_voxelEditStatus.empty())
    {
        ImGui::TextColored(WarningColor, "%s", m_voxelEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------------------------------------
// Baking

void Editor::StartVoxelBake(IDemoScene& scene, const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    m_voxelMessageIsError = true;

    if(m_voxelBake)
    {
        m_voxelMessage = "A voxel bake is already running.";
        return;
    }

    if(!owner || !owner->m_runtime || !m_historySession || !RenameSessionMatches(scene, *m_historySession))
    {
        m_voxelMessage = "Open a project scene to bake voxels.";
        return;
    }

    if(m_gameState != GameState::Editing || IsSceneEditingLocked() || HasPendingSceneHistoryOperation() || m_sceneOperationPending)
    {
        m_voxelMessage = "Voxels can be baked in a stopped scene without pending edits.";
        return;
    }

    const SceneDocument& document = owner->m_runtime->GetFileScene().GetDocument();
    // The snapshot is a copy: nothing done to the scene after this line reaches the bake.
    auto captured = VoxelScene::Capture(document, VoxelSceneAccess::Make(*owner->m_runtime, scene));

    if(!captured)
    {
        m_voxelMessage = captured.error();
        return;
    }

    if(captured->report.participating == 0)
    {
        m_voxelMessage = "No object participates: mark static meshes Static, or give them a Static physics body.";
        return;
    }

    VoxelScene::BakeJobOptions options;
    options.threads = (std::max)(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    options.memoryBudget = std::size_t{1} << 30;

    VoxelBakeRun run;
    run.session = *m_historySession;
    run.sceneBefore = document.voxelBake;
    run.settings = captured->settings;
    run.report = captured->report;
    run.started = std::chrono::steady_clock::now();
    auto store = std::make_shared<VoxelScene::ProjectAssetStore>(paths, registry, captured->currentManifest);
    run.job = std::make_unique<VoxelScene::SceneBakeJob>(std::move(*captured), std::move(store), options, m_backgroundTasks);
    m_voxelBake = std::move(run);
    m_voxelMessage = "Baking " + std::to_string(m_voxelBake->report.participating) + " objects (" + std::to_string(m_voxelBake->report.triangles) +
        " triangles).";
    m_voxelMessageIsError = false;
}

void Editor::FinishVoxelBake(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    VoxelBakeRun& run = *m_voxelBake;
    m_voxelMessageIsError = true;

    // Published only into the scene it captured, with the voxel settings it baked: anything else discards it (the job's
    // destructor removes every staged asset) and the scene keeps its manifest.
    if(!owner || !owner->m_runtime || !RenameSessionMatches(scene, run.session))
    {
        m_voxelBake.reset();
        m_voxelMessage = "The scene changed during the voxel bake; nothing was published.";
        return;
    }

    if(owner->m_runtime->GetFileScene().GetDocument().voxelBake != run.sceneBefore)
    {
        m_voxelBake.reset();
        m_voxelMessage = "The scene's voxel settings changed during the bake; nothing was published. Bake again.";
        return;
    }

    auto outcome = run.job->Finish();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
    const std::optional<VoxelBakeSettings> before = run.sceneBefore;
    m_voxelBake.reset();

    if(!outcome)
    {
        m_voxelMessage = outcome.error().message;
        return;
    }

    const VoxelBakeSettings after = VoxelScene::BakeSettingsWithManifest(outcome->settings, outcome->manifestReference);
    std::string error;

    if(!CommitVoxelBake(scene, before, after, &error))
    {
        // Registered but not referenced; cleanup (VX.6) removes it. The scene keeps its previous manifest.
        m_voxelMessage = "The bake was stored but could not be recorded in the scene: " + error;
        return;
    }

    const auto& s = outcome->statistics;
    char text[256];
    std::snprintf(text, sizeof(text), "Published in %.2f s: %zu populated and %zu solid regions, %zu baked, %zu reused. Content %s.", seconds,
        s.populated, s.uniformInside, s.baked, s.reused, HashText(outcome->contentHash).c_str());
    m_voxelMessage = text;
    m_voxelMessageIsError = false;
    m_voxelLastOutcome = std::move(*outcome);
}

void Editor::RefreshVoxelStatus(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(m_voxelStatusCheck)
    {
        if(m_voxelStatusCheck->IsFinished())
        {
            m_voxelStatus = m_voxelStatusCheck->Take();
            m_voxelStatusCheck.reset();
        }
        return;
    }

    if(!owner || !owner->m_runtime || m_gameState != GameState::Editing)
    {
        return;
    }

    const SceneDocument& document = owner->m_runtime->GetFileScene().GetDocument();
    const std::string manifest = document.voxelBake ? document.voxelBake->manifestReference : std::string{};

    if(!m_voxelStatusRequested && !m_voxelStatusPending && manifest == m_voxelStatusManifest)
    {
        return;
    }

    // A scene just opened has its meshes still loading: wait for them rather than report the bake out of date.
    const VoxelScene::SceneAccess access = VoxelSceneAccess::Make(*owner->m_runtime, scene);
    for(const SceneObjectDocument& object : document.objects)
    {
        if(VoxelScene::ResolveParticipation(object).participates && access.mesh(object) == nullptr)
        {
            VoxelScene::BakeStatus waiting;
            waiting.manifestReference = manifest;
            waiting.state = manifest.empty() ? VoxelScene::BakeState::NotBaked : VoxelScene::BakeState::Stale;
            waiting.detail = "Waiting for scene geometry to load.";
            m_voxelStatus = std::move(waiting);
            m_voxelStatusPending = true;
            return;
        }
    }

    m_voxelStatusPending = false;
    m_voxelStatusRequested = false;
    m_voxelStatusManifest = manifest;
    auto captured = VoxelScene::Capture(document, access);

    if(!captured)
    {
        VoxelScene::BakeStatus status;
        status.state = manifest.empty() ? VoxelScene::BakeState::NotBaked : VoxelScene::BakeState::Stale;
        status.manifestReference = manifest;
        status.detail = captured.error();
        m_voxelStatus = std::move(status);
        return;
    }

    // Read-only: the check only reads registered assets.
    auto store = std::make_shared<const VoxelScene::ProjectAssetStore>(paths, registry, manifest);
    m_voxelStatusCheck = std::make_unique<VoxelScene::BakeStatusCheck>(std::move(*captured), std::move(store));
}

void Editor::PollVoxelBake(IDemoScene& scene, const ProjectPaths& paths, ProjectAssetRegistry& registry)
{
    if(m_voxelBake)
    {
        m_voxelBake->job->Poll();

        // Published only when edits are allowed; until then the finished job waits with its assets staged.
        if(m_voxelBake->job->IsFinished() && !IsSceneEditingLocked() && !HasPendingSceneHistoryOperation() && !m_sceneOperationPending)
        {
            FinishVoxelBake(scene);
        }
    }
    else if(m_voxelBakeRequested)
    {
        m_voxelBakeRequested = false;
        StartVoxelBake(scene, paths, registry);
    }

    if(m_showVoxelWindow)
    {
        RefreshVoxelStatus(scene, paths, registry);
    }

    RefreshVoxelOverlay(scene, paths, registry);
}

void Editor::StopVoxelWork() noexcept
{
    m_voxelBakeRequested = false;
    m_voxelBake.reset();
    m_voxelStatusCheck.reset();
    m_voxelOverlayJob.reset();
}

// ---------------------------------------------------------------------------------------------------------------------------
// Voxels window

void Editor::DrawVoxelWindow(IDemoScene& scene)
{
    if(!m_showVoxelWindow)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460.0f, 560.0f), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("Voxels", &m_showVoxelWindow))
    {
        ImGui::End();
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !owner->m_runtime)
    {
        ImGui::TextDisabled("Open a project scene to bake voxels.");
        ImGui::End();
        return;
    }

    const SceneDocument& document = owner->m_runtime->GetFileScene().GetDocument();
    const std::optional<VoxelBakeSettings> current = document.voxelBake;
    const VoxelBakeSettings settings = current.value_or(VoxelBakeSettings{});
    const bool baking = m_voxelBake.has_value();
    const bool editingBlocked = baking || IsSceneEditingLocked() || m_gameState != GameState::Editing;

    // Settings: each change is one history entry and keeps the manifest, whose status then reads "Settings changed".
    ImGui::SeparatorText("Settings");
    ImGui::BeginDisabled(editingBlocked);
    std::string error;
    const auto commitSettings = [&](auto change) {
        VoxelBakeSettings after = settings;
        change(after);
        if(!CommitVoxelBake(scene, current, after, &error))
        {
            m_voxelMessage = error;
            m_voxelMessageIsError = true;
        }
    };

    if(ImGui::BeginCombo("Voxel size", MetresText(settings.baseExponent).c_str()))
    {
        for(int exponent = -7; exponent <= 4; ++exponent)
        {
            if(ImGui::Selectable(MetresText(exponent).c_str(), exponent == settings.baseExponent))
            {
                commitSettings([&](VoxelBakeSettings& s) { s.baseExponent = exponent; });
            }
        }
        ImGui::EndCombo();
    }

    const auto regionText = [&](std::uint32_t bricks) {
        char text[64];
        std::snprintf(text, sizeof(text), "%u bricks (%g m)", bricks, std::ldexp(static_cast<double>(bricks) * 8.0, settings.baseExponent));
        return std::string(text);
    };

    if(ImGui::BeginCombo("Region size", regionText(settings.regionBricks).c_str()))
    {
        for(const std::uint32_t bricks : {16u, 32u, 64u})
        {
            if(ImGui::Selectable(regionText(bricks).c_str(), bricks == settings.regionBricks))
            {
                commitSettings([&](VoxelBakeSettings& s) { s.regionBricks = bricks; });
            }
        }
        ImGui::EndCombo();
    }

    // Sliders commit on release, so one drag is one history entry.
    int band = static_cast<int>(settings.narrowBand);
    ImGui::SliderInt("Narrow band (voxels)", &band, 1, 32);
    if(ImGui::IsItemDeactivatedAfterEdit())
    {
        commitSettings([&](VoxelBakeSettings& s) { s.narrowBand = static_cast<std::uint32_t>(std::clamp(band, 1, 32)); });
    }

    int clamp = static_cast<int>(settings.coarseClamp);
    ImGui::SliderInt("Coarse distance (bricks)", &clamp, 1, 16);
    if(ImGui::IsItemDeactivatedAfterEdit())
    {
        commitSettings([&](VoxelBakeSettings& s) { s.coarseClamp = static_cast<std::uint32_t>(std::clamp(clamp, 1, 16)); });
    }

    ImGui::EndDisabled();

    // Bake.
    ImGui::SeparatorText("Bake");

    if(!baking)
    {
        ImGui::BeginDisabled(editingBlocked || m_voxelBakeRequested);
        if(ImGui::Button("Bake Voxels"))
        {
            m_voxelBakeRequested = true;
        }
        ImGui::EndDisabled();
    }
    else
    {
        const auto [done, total] = m_voxelBake->job->Poll();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_voxelBake->started).count();
        char overlay[96];
        if(total == 0)
        {
            std::snprintf(overlay, sizeof(overlay), "Planning... %.1f s", seconds);
        }
        else if(m_voxelBake->job->IsFinished())
        {
            std::snprintf(overlay, sizeof(overlay), "Publishing when the scene is editable");
        }
        else
        {
            std::snprintf(overlay, sizeof(overlay), "%zu / %zu regions, %.1f s", done, total, seconds);
        }
        ImGui::ProgressBar(total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(-1.0f, 0.0f), overlay);
        if(ImGui::Button("Cancel"))
        {
            m_voxelBake->job->Cancel();
        }
    }

    if(!m_voxelMessage.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(m_voxelMessageIsError ? ErrorColor : GoodColor, "%s", m_voxelMessage.c_str());
        ImGui::PopTextWrapPos();
    }

    // The published bake.
    ImGui::SeparatorText("Current bake");

    if(settings.manifestReference.empty())
    {
        ImGui::TextDisabled("Not baked.");
    }
    else
    {
        ImGui::TextUnformatted(settings.manifestReference.c_str());
    }

    if(m_voxelStatusCheck)
    {
        ImGui::TextDisabled("Checking...");
    }
    else if(m_voxelStatusPending)
    {
        ImGui::TextDisabled("Waiting for scene geometry to load...");
    }
    else if(m_voxelStatus && m_voxelStatus->manifestReference == settings.manifestReference)
    {
        const VoxelScene::BakeStatus& status = *m_voxelStatus;
        ImGui::TextColored(StateColor(status.state), "%s", VoxelScene::BakeStateToString(status.state));
        if(status.contentHash)
        {
            ImGui::Text("Content %s, %zu populated and %zu solid regions", HashText(*status.contentHash).c_str(), status.populated, status.uniformInside);
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", status.detail.c_str());
        ImGui::PopTextWrapPos();
    }

    if(ImGui::Button("Check Status"))
    {
        m_voxelStatusRequested = true;
    }

    // Participation, from the policy, as it stands now.
    ImGui::SeparatorText("Participation");
    std::vector<std::pair<const SceneObjectDocument*, VoxelScene::ParticipationDecision>> decisions;
    std::size_t participating = 0;
    std::size_t warnings = 0;

    for(const SceneObjectDocument& object : document.objects)
    {
        if(object.meshReference.empty())
        {
            continue;
        }

        const VoxelScene::ParticipationDecision decision = VoxelScene::ResolveParticipation(object);
        participating += decision.participates;
        warnings += decision.warning;
        decisions.emplace_back(&object, decision);
    }

    ImGui::Text("%zu of %zu mesh objects bake", participating, decisions.size());
    if(warnings > 0)
    {
        ImGui::TextColored(WarningColor, "%zu marked Static on a moving body (not baked)", warnings);
    }

    if(ImGui::BeginTable("VoxelParticipation", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
           ImVec2(0.0f, 200.0f)))
    {
        ImGui::TableSetupColumn("Object");
        ImGui::TableSetupColumn("Voxels");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for(const auto& [object, decision] : decisions)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(object->name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(decision.warning ? WarningColor : (decision.participates ? GoodColor : DimColor), "%s%s",
                decision.participates ? "Bakes: " : "", VoxelScene::ParticipationReasonToString(decision.reason));
        }

        ImGui::EndTable();
    }

    DrawVoxelOverlayControls();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------------------------------------
// Voxel overlay (VX.4C.2)

namespace
{
    std::string RegionKeyText(const Voxel::RegionKey& key)
    {
        return "(" + std::to_string(key.x) + ", " + std::to_string(key.y) + ", " + std::to_string(key.z) + ")";
    }

    // What a build is for: the manifest and the isolated region. An empty manifest means there is nothing to show.
    std::string OverlayIdentity(const std::string& manifest, const std::optional<Voxel::RegionKey>& isolate)
    {
        if(manifest.empty())
        {
            return {};
        }

        return isolate ? manifest + " " + RegionKeyText(*isolate) : manifest;
    }

    constexpr std::size_t ListedRegionErrors = 8;
}

void Editor::ResetVoxelOverlay() noexcept
{
    m_voxelOverlayJob.reset();
    m_voxelOverlayJobFor.clear();
    m_voxelOverlayResult.reset();
    m_voxelOverlayBuiltFor.clear();
    m_voxelOverlayIsolate.reset();
    m_voxelOverlayError.clear();
    // Whatever the Renderer holds belongs to the previous state; a pending publication is superseded.
    m_voxelOverlayUpload = VoxelOverlayUpload::Clear;
}

void Editor::RefreshVoxelOverlay(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(!m_voxelOverlayVisible)
    {
        if(m_voxelOverlayJob || m_voxelOverlayResult || m_voxelOverlayGeneration != 0)
        {
            const std::optional<Voxel::RegionKey> isolate = m_voxelOverlayIsolate;
            ResetVoxelOverlay();
            m_voxelOverlayIsolate = isolate;
        }

        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner && owner->m_runtime ? &owner->m_runtime->GetFileScene().GetDocument() : nullptr;
    const std::string manifest = document && document->voxelBake ? document->voxelBake->manifestReference : std::string{};
    const std::string wanted = OverlayIdentity(manifest, m_voxelOverlayIsolate);

    // A finished build is used only if it is still what is wanted: a bake, undo, redo or new selection may have overtaken it.
    if(m_voxelOverlayJob)
    {
        if(!m_voxelOverlayJob->IsFinished() && m_voxelOverlayJobFor == wanted)
        {
            return;
        }

        if(m_voxelOverlayJob->IsFinished() && m_voxelOverlayJobFor == wanted)
        {
            VoxelVisualization::Result result = m_voxelOverlayJob->Take();
            m_voxelOverlayJob.reset();
            const bool drawable = result.error.empty() && !result.cancelled && !result.data.regions.empty();
            m_voxelOverlayWorldFromLattice = VoxelVisualization::WorldFromLattice(result);
            m_voxelOverlayResult = std::move(result);
            m_voxelOverlayBuiltFor = wanted;
            m_voxelOverlayUpload = drawable ? VoxelOverlayUpload::Publish : VoxelOverlayUpload::Clear;
            return;
        }

        // Superseded: destroying the job cancels it (checked per row of bricks) and joins.
        m_voxelOverlayJob.reset();
    }

    if(wanted == m_voxelOverlayBuiltFor)
    {
        return;
    }

    // The previous overlay is withdrawn at once, so an outdated bake is never shown while the new one builds.
    m_voxelOverlayResult.reset();
    m_voxelOverlayBuiltFor.clear();
    m_voxelOverlayError.clear();
    m_voxelOverlayUpload = VoxelOverlayUpload::Clear;

    if(wanted.empty())
    {
        return;
    }

    // Read-only: the build reads registered assets through paths captured here, never the live registry.
    auto store = std::make_shared<const VoxelScene::ProjectAssetStore>(paths, registry, manifest);
    m_voxelOverlayJob = std::make_unique<VoxelVisualization::Job>(std::move(store), manifest, m_voxelOverlayIsolate);
    m_voxelOverlayJobFor = wanted;
}

void Editor::PublishVoxelOverlay(const std::function<std::uint64_t(const EditorVoxelOverlay::Data&, std::string*)>& publish,
    const std::function<void()>& clear)
{
    const VoxelOverlayUpload upload = m_voxelOverlayUpload;
    m_voxelOverlayUpload = VoxelOverlayUpload::None;

    if(upload == VoxelOverlayUpload::Clear)
    {
        clear();
        m_voxelOverlayGeneration = 0;
        return;
    }

    if(upload != VoxelOverlayUpload::Publish || !m_voxelOverlayResult)
    {
        return;
    }

    std::string error;
    m_voxelOverlayGeneration = publish(m_voxelOverlayResult->data, &error);

    if(m_voxelOverlayGeneration == 0)
    {
        // The Renderer kept whatever it had; withdraw that too, so nothing stale is drawn.
        clear();
        m_voxelOverlayError = "The voxels could not be shown: " + error;
    }

    // The GPU now holds the cubes; only the region table is kept, for the window.
    m_voxelOverlayResult->data.cells = {};
}

std::optional<EditorVoxelOverlay::Request> Editor::GetVoxelOverlayRequest() const
{
    if(!m_voxelOverlayVisible || m_voxelOverlayGeneration == 0)
    {
        return std::nullopt;
    }

    EditorVoxelOverlay::Request request;
    request.generation = m_voxelOverlayGeneration;
    request.worldFromLattice = m_voxelOverlayWorldFromLattice;
    request.drawBoxes = m_voxelOverlayBoxes;
    return request;
}

void Editor::DrawVoxelOverlayControls()
{
    ImGui::SeparatorText("Visualization");
    ImGui::Checkbox("Show voxels", &m_voxelOverlayVisible);
    ImGui::SameLine();
    ImGui::Checkbox("Region boxes", &m_voxelOverlayBoxes);

    if(!m_voxelOverlayVisible)
    {
        ImGui::TextDisabled("Shows the published bake in the viewport.");
        return;
    }

    ImGui::TextDisabled("Cubes: orange surface, blue interior. Boxes: grey populated, blue uniform inside, red error.");

    if(m_voxelOverlayJob)
    {
        ImGui::TextUnformatted("Building the voxel overlay...");
    }

    if(!m_voxelOverlayError.empty())
    {
        ImGui::TextColored(ErrorColor, "%s", m_voxelOverlayError.c_str());
    }

    if(!m_voxelOverlayResult)
    {
        if(!m_voxelOverlayJob)
        {
            ImGui::TextDisabled("The scene has no published voxel bake.");
        }

        return;
    }

    const VoxelVisualization::Result& result = *m_voxelOverlayResult;

    if(!result.error.empty())
    {
        ImGui::TextColored(ErrorColor, "%s", result.error.c_str());
        return;
    }

    // Region selection: all regions, or one isolated. The list is clipped, so a large manifest stays responsive.
    const std::string current = m_voxelOverlayIsolate ? "Region " + RegionKeyText(*m_voxelOverlayIsolate) : std::string("All regions");

    if(ImGui::BeginCombo("Regions", current.c_str(), ImGuiComboFlags_HeightLarge))
    {
        if(ImGui::Selectable("All regions", !m_voxelOverlayIsolate))
        {
            m_voxelOverlayIsolate.reset();
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(result.regions.size()));

        while(clipper.Step())
        {
            for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const VoxelVisualization::RegionSummary& region = result.regions[static_cast<std::size_t>(row)];
                const std::string kind = region.uniformInside ? "uniform inside" : std::to_string(region.bricks) + " bricks";
                const std::string label = RegionKeyText(region.key) + "  " + kind;
                const bool selected = m_voxelOverlayIsolate && *m_voxelOverlayIsolate == region.key;

                ImGui::PushID(row);

                if(ImGui::Selectable(label.c_str(), selected))
                {
                    m_voxelOverlayIsolate = region.key;
                }

                ImGui::PopID();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Text("%zu of %zu regions drawn: %llu surface and %llu interior voxels.", result.data.regions.size(), result.regions.size(),
        static_cast<unsigned long long>(result.surfaceVoxels), static_cast<unsigned long long>(result.interiorVoxels));

    if(result.truncated)
    {
        const VoxelVisualization::RegionSummary& first = result.regions[result.firstUndrawnRegion];
        ImGui::TextColored(WarningColor, "Cube limit (%zu) reached: from region %s on, regions are boxed but not fully drawn.",
            EditorVoxelOverlay::MaximumCells, RegionKeyText(first.key).c_str());

        if(!m_voxelOverlayIsolate)
        {
            ImGui::TextColored(WarningColor, "Isolate a region to see it completely.");
        }
    }

    if(!result.regionErrors.empty())
    {
        ImGui::TextColored(ErrorColor, "%zu regions could not be shown (red boxes):", result.regionErrors.size());

        for(std::size_t index = 0; index < result.regionErrors.size() && index < ListedRegionErrors; ++index)
        {
            const VoxelVisualization::RegionError& regionError = result.regionErrors[index];
            ImGui::TextColored(ErrorColor, "  %s: %s", RegionKeyText(regionError.key).c_str(), regionError.message.c_str());
        }
    }
}
