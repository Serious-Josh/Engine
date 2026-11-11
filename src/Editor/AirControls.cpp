// AIR.4A/AIR.4B: the Editor's Air window (Editor -> Air). Session settings for the camera-centred air simulation and its velocity-vector
// view; the Engine applies them every frame (Engine::Frame). AIR.4F F4.4.1: except the mist's appearance (Mist rendering), which is
// scene data (SceneDocument::mistRendering), edited through scene history.

#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace
{
// Keeps a dragged value finite and inside its range; the window never hands the Engine a value it would refuse.
void Clamp(float& value, float minimum, float maximum)
{
    value = std::isfinite(value) ? std::clamp(value, minimum, maximum) : minimum;
}
}

void Editor::DrawAirWindow(IDemoScene& scene)
{
    if(!m_showAirWindow)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("Air", &m_showAirWindow))
    {
        ImGui::End();
        return;
    }

    AirEditorSettings& air = m_airSettings;

    ImGui::SeparatorText("Simulation");
    ImGui::Checkbox("Simulate air", &air.enabled);
    ImGui::BeginDisabled(!air.enabled);

    if(ImGui::Button(air.paused ? "Resume" : "Pause"))
    {
        air.paused = !air.paused;
    }

    ImGui::SameLine();

    if(ImGui::Button("Reset"))
    {
        m_airResetRequested = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", !air.enabled ? "Off" : (air.paused ? "Paused" : "Running"));

    float ambient[3] = {air.ambientVelocity.x, air.ambientVelocity.y, air.ambientVelocity.z};

    if(ImGui::DragFloat3("Ambient wind (m/s)", ambient, 0.05f, -50.0f, 50.0f, "%.2f"))
    {
        for(float& component : ambient)
        {
            Clamp(component, -50.0f, 50.0f);
        }

        air.ambientVelocity = {ambient[0], ambient[1], ambient[2]};
    }

    if(ImGui::DragFloat("Damping (1/s)", &air.dampingRate, 0.01f, 0.0f, 20.0f, "%.2f"))
    {
        Clamp(air.dampingRate, 0.0f, 20.0f);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How fast air relaxes to the ambient wind: the difference shrinks by e^(-damping x seconds).");
    }

    if(ImGui::DragFloat("Motor speed cap (m/s)", &air.maximumMotorSpeed, 0.1f, 0.0f, 200.0f, "%.1f"))
    {
        Clamp(air.maximumMotorSpeed, 0.0f, 200.0f);
    }

    // AIR.4F: the passive mist. It is simulated only while an enabled Mist Emitter feeds it (Add Component -> Mist Emitter).
    ImGui::SeparatorText("Mist");
    ImGui::Checkbox("Show mist", &air.showMist);
    ImGui::SameLine();

    if(ImGui::Button(air.mistPaused ? "Resume mist" : "Pause mist"))
    {
        air.mistPaused = !air.mistPaused;
    }

    ImGui::SameLine();

    if(ImGui::Button("Clear mist"))
    {
        m_mistClearRequested = true;
    }

    if(ImGui::DragFloat("Mist fade (1/s)", &air.mistDissipationRate, 0.01f, 0.0f, 10.0f, "%.2f"))
    {
        Clamp(air.mistDissipationRate, 0.0f, 10.0f);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How fast mist fades: its density shrinks by e^(-fade x seconds). Mist exists only while an enabled Mist\n"
            "Emitter feeds it; it is carried by the simulated air and never crosses solid geometry.");
    }

    ImGui::BeginDisabled(!air.showMist);

    static constexpr const char* QualityNames[] = {"Low (1/4 res, 32 steps)", "Medium (1/4 res, 64 steps)", "High (1/2 res, 96 steps)"};
    int quality = std::clamp(air.mistQuality, 0, 2);

    if(ImGui::Combo("Mist quality", &quality, QualityNames, IM_ARRAYSIZE(QualityNames)))
    {
        air.mistQuality = quality;
    }

    ImGui::EndDisabled();
    DrawMistRenderingControls(scene);

    ImGui::SeparatorText("Velocity vectors");
    ImGui::Checkbox("Show vectors", &air.showVectors);
    ImGui::BeginDisabled(!air.showVectors);
    // AIR.4D D3: where vectors start.
    static constexpr const char* PlacementNames[] = {"Spread points", "At piece anchors"};
    int placement = air.vectorsAtPieceAnchors ? 1 : 0;

    if(ImGui::Combo("Placement", &placement, PlacementNames, IM_ARRAYSIZE(PlacementNames)))
    {
        air.vectorsAtPieceAnchors = placement == 1;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Spread points: one vector per spacing block, at a fixed point inside it, showing the air of whichever fluid\n"
            "piece holds the point. At piece anchors: one vector for each fluid piece of a cell (two where a thin wall splits it),\n"
            "where the simulation keeps that piece's velocity; the spacing is at least one cell.");
    }

    // Spacing in powers of two from a quarter of a cell to 8 cells; finer than a cell blends the surrounding cells.
    static constexpr const char* SpacingNames[] = {"1/4 cell (4 per cell)", "1/2 cell (2 per cell)", "1 cell", "2 cells", "4 cells", "8 cells"};
    int spacing = std::clamp(air.vectorSpacingExponent, -2, 3) + 2;

    if(ImGui::Combo("Spacing", &spacing, SpacingNames, IM_ARRAYSIZE(SpacingNames)))
    {
        air.vectorSpacingExponent = spacing - 2;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Distance between vectors along each axis. Velocity comes from the fluid piece holding each vector's start,\n"
            "never blended across walls or from another piece; the simulation itself is unchanged.");
    }

    if(ImGui::SliderInt("Vector budget", &air.vectorBudget, 1'024, 4'194'304, "%d", ImGuiSliderFlags_Logarithmic))
    {
        air.vectorBudget = std::clamp(air.vectorBudget, 1'024, 4'194'304);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The most vectors drawn (at piece anchors, both of a cell's pieces count). When the spacing would need more,\n"
            "only a box of vectors around the camera is drawn.");
    }

    if(ImGui::DragFloat("Length scale (m per m/s)", &air.vectorLengthScale, 0.005f, 0.005f, 2.0f, "%.3f"))
    {
        Clamp(air.vectorLengthScale, 0.005f, 2.0f);
    }

    if(ImGui::DragFloat("Maximum length (m)", &air.vectorMaximumLength, 0.05f, 0.05f, 10.0f, "%.2f"))
    {
        Clamp(air.vectorMaximumLength, 0.05f, 10.0f);
    }

    if(ImGui::DragFloat("Red at speed (m/s)", &air.colorMaximumSpeed, 0.1f, 0.1f, 200.0f, "%.1f"))
    {
        Clamp(air.colorMaximumSpeed, 0.1f, 200.0f);
    }

    if(ImGui::DragFloat("Hide below (m/s)", &air.minimumSpeed, 0.005f, 0.0f, 5.0f, "%.3f"))
    {
        Clamp(air.minimumSpeed, 0.0f, 5.0f);
    }

    ImGui::TextDisabled("Blue (slow) to cyan, yellow and red (fast); the bright end points downwind.");
    ImGui::EndDisabled();

    // The air field's own obstacle mask, at the cells' exact bounds, to compare with the geometry.
    ImGui::SeparatorText("Obstacle cells");
    ImGui::Checkbox("Blocked", &air.showBlockedCells);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Air cells the simulation treats as solid: any cell a baked voxel obstacle touches. Drawn in a box around the camera.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Unknown", &air.showUnknownCells);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Air cells with no voxel data (unbaked, or not resident): the simulation treats them as free air.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Fluid pieces", &air.showFluidPieces);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The finer boundary each cell's air sees, at a quarter of a cell: teal borders piece 0, yellow piece 1, and\n"
            "magenta a further piece the cell cannot hold. The simulation steps each piece separately.");
    }

    ImGui::EndDisabled();
    ImGui::End();
}

MistRenderSettings Editor::GetEffectiveMistRendering(const SceneDocument* document, std::uint64_t runtimeIdentity) const
{
    if(m_mistRenderingDraft && m_mistRenderingDraft->runtimeIdentity == runtimeIdentity && document != nullptr)
    {
        return m_mistRenderingDraft->settings;
    }

    return document != nullptr ? document->mistRendering.value_or(MistRenderSettings{}) : MistRenderSettings{};
}

size_t Editor::MistRenderingEditPayloadBytes(const MistRenderingEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(MistRenderingEdit) + edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath);
}

// One history entry for the scene's mist appearance. The first edit of a scene without the block stores it (from its defaults); undo
// returns the scene to having none.
bool Editor::CommitMistRendering(IDemoScene& scene, const std::optional<MistRenderSettings>& before, const MistRenderSettings& after,
    std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "The mist's appearance can be changed in a stopped scene without pending edits.";
        }

        return false;
    }

    if(before.value_or(MistRenderSettings{}) == after)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<MistRenderingEdit>(MistRenderingEdit{*m_historySession, before, after});
        const size_t payload = MistRenderingEditPayloadBytes(*command);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneMistRendering(before, after, error))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
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

bool Editor::ReplayMistRendering(IDemoScene& scene, const MistRenderingEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Mist rendering replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneMistRendering(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

// AIR.4F F4.4.1: the scene's mist appearance. A checkbox commits at once; a slider shows its value live (a draft) and commits on release,
// so one drag is one history entry.
void Editor::DrawMistRenderingControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner && owner->m_runtime && !owner->IsArchiveActivation() ? &owner->m_runtime->GetFileScene().GetDocument() :
        nullptr;

    const std::uint64_t runtimeIdentity = document != nullptr ? owner->GetRuntimeIdentity() : 0;

    if(m_mistRenderingDraft && (document == nullptr || m_mistRenderingDraft->runtimeIdentity != runtimeIdentity))
    {
        m_mistRenderingDraft.reset();
    }

    ImGui::SeparatorText("Mist rendering (scene)");

    if(document == nullptr)
    {
        ImGui::TextDisabled("Open a project scene to edit the mist's appearance.");
        return;
    }

    const std::optional<MistRenderSettings> stored = document->mistRendering;
    MistRenderSettings settings = GetEffectiveMistRendering(document, runtimeIdentity);
    const bool editable = !IsSceneEditingLocked() && m_gameState == GameState::Editing;
    bool changed = false;
    bool commit = false;
    // A checkbox is a whole edit; a slider is one while it is held.
    const auto checkbox = [&](const char* label, bool& value)
    {
        if(ImGui::Checkbox(label, &value))
        {
            changed = true;
            commit = true;
        }
    };
    const auto slider = [&](bool edited)
    {
        changed = changed || edited;
        commit = commit || ImGui::IsItemDeactivatedAfterEdit();
    };

    ImGui::BeginDisabled(!editable);
    slider(ImGui::DragFloat("Mist opacity", &settings.opacity, 0.01f, 0.0f, MistRenderSettings::MaximumOpacity, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How much light the mist intercepts per metre at a given density (its extinction).");
    }

    // AIR.4F F4.3: the mist scatters the scene's light (off: it shows its own colour, unlit).
    checkbox("Lighting enabled", settings.lighting);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("On: the mist is lit by the scene's sun and Lights, scattering their light toward the camera in the mist's\n"
            "colour. Off: the mist shows its own colour, unaffected by lighting.");
    }

    ImGui::BeginDisabled(!settings.lighting);
    checkbox("Sun shadows", settings.sunShadows);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Geometry between the sun and the mist shadows it (resolved per half-metre air cell, so shadow edges in the\n"
            "mist are soft). Point and Spot Lights are not shadowed, as on surfaces.");
    }

    slider(ImGui::SliderFloat("Scattering strength", &settings.scattering, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The share of the light the mist intercepts that it scatters rather than absorbs (its albedo, times the mist's\n"
            "colour). 1 is a white, non-absorbing mist like fog; lower values make it darker and smokier. How much light the mist\n"
            "intercepts is set by Mist opacity.");
    }

    slider(ImGui::SliderFloat("Scattering anisotropy", &settings.anisotropy, -MistRenderSettings::MaximumAnisotropy,
        MistRenderSettings::MaximumAnisotropy, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Henyey-Greenstein g: above 0 the mist glows most looking toward a light, below 0 looking away; 0 scatters\n"
            "evenly in every direction.");
    }

    slider(ImGui::SliderFloat("Ambient contribution", &settings.ambient, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The share of the environment's (sky's) light that reaches the mist. It is not shadowed, so 1 suits open\n"
            "outdoor mist and indoor mist needs much less; it lights the mist evenly from every side, which flattens it. 0 adds\n"
            "none: with no Lights the mist is then dark.");
    }

    ImGui::EndDisabled();

    // AIR.4F F4.1: cloudy detail carried by the simulated wind; it only modulates where mist already is.
    checkbox("Worley detail", settings.detail);
    ImGui::BeginDisabled(!settings.detail);
    slider(ImGui::SliderFloat("Detail strength", &settings.detailStrength, 0.0f, 1.0f, "%.2f"));
    slider(ImGui::DragFloat("Detail scale (m)", &settings.detailScale, 0.05f, MistRenderSettings::MinimumDetailScale,
        MistRenderSettings::MaximumDetailScale, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The size of one repeat of the noise pattern. The pattern moves with the local simulated wind and never\n"
            "adds mist where the simulation has none; its average density is unchanged.");
    }

    // AIR.4F F4.4: curl noise warps the Worley pattern into swirls; a visual effect only.
    checkbox("Curl noise", settings.curl);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Swirls the Worley detail along a divergence-free curl field, for more turbulent, directional structure. It\n"
            "only reshapes how the detail is drawn: the mist itself, the air and the mist's movement are unchanged (it is not\n"
            "turbulence).");
    }

    ImGui::BeginDisabled(!settings.curl);
    slider(ImGui::SliderFloat("Curl strength (m)", &settings.curlStrength, 0.0f, MistRenderSettings::MaximumCurlStrength, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How far the curl field moves the detail pattern, in metres (its typical displacement). 0 leaves the Worley\n"
            "detail unchanged; about a tenth of the detail scale gives soft swirls, a quarter or more strong distortion.");
    }

    slider(ImGui::DragFloat("Curl scale (m)", &settings.curlScale, 0.05f, MistRenderSettings::MinimumCurlScale, MistRenderSettings::MaximumCurlScale,
        "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The size of one repeat of the curl field: larger values give broader, slower-changing swirls.");
    }

    slider(ImGui::SliderFloat("Curl flow influence", &settings.curlFlow, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How far the swirls ride the local wind: 1 moves them with the air like the Worley pattern, 0 holds them in\n"
            "place so the detail streams through them. Still air leaves them still either way.");
    }

    ImGui::EndDisabled();

    // AIR.4F F4.4.2: wisps: curling strands multiplying the detail; a visual effect only.
    checkbox("Wisps", settings.wisps);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Adds wispy strands that curl along a swirling pattern and drift with the local wind; in still air they stay\n"
            "put. They only reshape how the mist's density is drawn where there is mist: the simulation, the air and the mist's\n"
            "movement are unchanged.");
    }

    ImGui::BeginDisabled(!settings.wisps);
    slider(ImGui::SliderFloat("Wisp strength", &settings.wispStrength, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How strongly the strands shape the mist: 0 leaves the detail unchanged; higher values concentrate the mist\n"
            "into the strands and clear the space between them.");
    }

    slider(ImGui::DragFloat("Wisp scale (m)", &settings.wispScale, 0.05f, MistRenderSettings::MinimumWispScale, MistRenderSettings::MaximumWispScale,
        "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The size of one repeat of the strand pattern: larger values give longer, broader strands.");
    }

    ImGui::EndDisabled();

    // AIR.4F F4.4.2e: density-dependent detail; a visual effect only.
    checkbox("Density-dependent detail", settings.densityDetail);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Varies the detail with how thick the mist looks: thin mist shows finer, sharper strands, dense mist\n"
            "softer, broader shapes. The amount of mist, its opacity and its lighting are unchanged on average.");
    }

    ImGui::BeginDisabled(!settings.densityDetail);
    slider(ImGui::SliderFloat("Density detail strength", &settings.densityDetailStrength, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How strongly the detail follows the mist's thickness: 0 looks exactly as without it.");
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if(!editable)
    {
        ImGui::TextDisabled("The mist's appearance is edited in a stopped scene.");
    }

    if(!m_mistRenderingStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_mistRenderingStatus.c_str());
    }

    if(!editable || !changed)
    {
        return;
    }

    settings = SanitizeMistRenderSettings(settings);
    m_mistRenderingDraft = MistRenderingDraft{runtimeIdentity, settings};

    if(!commit)
    {
        return;
    }

    std::string error;
    m_mistRenderingDraft.reset();
    m_mistRenderingStatus = CommitMistRendering(scene, stored, settings, &error) ? std::string{} : error;
}
