// AIR.4A/AIR.4B: the Editor's Air window (Editor -> Air). Session settings for the camera-centred air simulation's pause, views and
// diagnostics; the Engine applies them every frame (Engine::Frame). Scene data, edited through scene history: the mist's appearance
// (AIR.4F F4.4.1, SceneDocument::mistRendering), the air environment and, from R1.1, the simulation configuration
// (SceneDocument::airSimulation; a session override while Play or a simulation runs).

#include "Editor/Editor.h"
#include "Editor/MistAppearanceFields.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Debug/DebugUI.h"
#include "Engine/RenderingOverrides.h"

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

void Editor::DrawAirWindow(IDemoScene& scene, DebugUI& debugUI)
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

    // R1.1: the simulation configuration is the scene's (a session override while Play or a simulation runs). Its controls edit a copy;
    // ApplyAirSimulationEdit routes the result once the window is drawn. A checkbox, combo or button is a whole edit; a drag is one
    // while it is held.
    const AirSimulationSettings displayed = GetDisplayedAirSimulation(scene);
    AirSimulationSettings simulation = displayed;
    bool simulationCommit = false;
    const auto simulationDrag = [&]()
    {
        simulationCommit = simulationCommit || ImGui::IsItemDeactivatedAfterEdit();
    };
    const auto simulationCheckbox = [&](const char* label, bool& value)
    {
        if(ImGui::Checkbox(label, &value))
        {
            simulationCommit = true;
        }
    };
    auto* simulationOwner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const bool hasSceneDocument = simulationOwner && simulationOwner->m_runtime && !simulationOwner->IsArchiveActivation();
    const bool sessionOverrideMode = IsSceneEditingLocked();
    // W3: an assigned Local Level's AIR simulation is its World's (edited in World Membership); its wind override is edited in the
    // Environment (Local Level) panel. Play and Physics Simulation session overrides are unchanged.
    const bool worldSimulation = hasSceneDocument &&
        simulationOwner->GetEffectiveEnvironment().source == EffectiveSceneEnvironment::Source::AssignedWorld;
    const bool simulationEditable = sessionOverrideMode || (hasSceneDocument && m_gameState == GameState::Editing && !worldSimulation);

    ImGui::SeparatorText("Simulation");

    if(worldSimulation && !sessionOverrideMode)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "AIR simulation belongs to World '%s' (edit it in World Membership). "
            "Wind is set in Environment (Local Level).", simulationOwner->GetEffectiveEnvironment().worldName.c_str());
    }

    if(sessionOverrideMode)
    {
        const bool overridden = simulationOwner && GetAirSimulationOverride(simulationOwner->GetRuntimeIdentity()).has_value();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", overridden ? "Session override (not saved)" :
            "Scene settings; changes here are a session override (not saved)");
    }
    else if(!hasSceneDocument)
    {
        ImGui::TextDisabled("Open a project scene to edit the AIR simulation settings.");
    }
    else
    {
        ImGui::TextDisabled("Scene settings (saved with the scene; undoable)");
    }

    if(!m_airSimulationStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_airSimulationStatus.c_str());
    }

    ImGui::BeginDisabled(!simulationEditable);
    simulationCheckbox("Simulate air", simulation.enabled);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!simulation.enabled);

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
    ImGui::TextDisabled("%s", !simulation.enabled ? "Off" : (air.paused ? "Paused" : "Running"));

    // AIR.4E.5b.1: the most fixed steps a frame may run; the step itself stays 1/60 s.
    static constexpr const char* StepLimitNames[] = {"1", "2"};
    int stepLimit = simulation.maximumStepsPerFrame == 1 ? 0 : 1;
    ImGui::BeginDisabled(!simulationEditable);

    if(ImGui::Combo("Max AIR steps / frame", &stepLimit, StepLimitNames, IM_ARRAYSIZE(StepLimitNames)))
    {
        simulation.maximumStepsPerFrame = stepLimit + 1;
        simulationCommit = true;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The most 1/60 s air steps a rendered frame may run. With 1, frames slower than 60 per second run one step each,\n"
            "so the air moves slower than real time; time beyond the limit is dropped, never caught up later.");
    }

    ImGui::EndDisabled();
    ImGui::TextDisabled("Steps last frame: %u of at most %d; step %.3f ms", simulation.enabled ? m_airStepCount : 0u,
        simulation.maximumStepsPerFrame == 1 ? 1 : 2, 1000.0 / 60.0);

    ImGui::BeginDisabled(!simulationEditable);
    const float ambientLimit = AirSimulationSettings::MaximumAmbientSpeed;
    float ambient[3] = {simulation.ambientVelocity.x, simulation.ambientVelocity.y, simulation.ambientVelocity.z};

    if(ImGui::DragFloat3("Ambient wind (m/s)", ambient, 0.05f, -ambientLimit, ambientLimit, "%.2f"))
    {
        for(float& component : ambient)
        {
            Clamp(component, -ambientLimit, ambientLimit);
        }

        simulation.ambientVelocity = {ambient[0], ambient[1], ambient[2]};
    }

    simulationDrag();

    if(ImGui::DragFloat("Damping (1/s)", &simulation.dampingRate, 0.01f, 0.0f, AirSimulationSettings::MaximumDampingRate, "%.2f"))
    {
        Clamp(simulation.dampingRate, 0.0f, AirSimulationSettings::MaximumDampingRate);
    }

    simulationDrag();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How fast air relaxes to the ambient wind: the difference shrinks by e^(-damping x seconds).");
    }

    // AIR.4F.1c: presets for the damping above (they only set it, nothing else).
    if(ImGui::SmallButton("Low Damping (Convection)"))
    {
        simulation.dampingRate = AirSimulationSettings::ConvectionDampingRate;
        simulationCommit = true;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Sets damping to 0.1/s, so buoyant plumes rise faster and spread further. Does not turn on buoyancy or pressure\n"
            "projection.");
    }

    ImGui::SameLine();

    if(ImGui::SmallButton("Default Damping"))
    {
        simulation.dampingRate = AirSimulationSettings::DefaultDampingRate;
        simulationCommit = true;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Sets damping back to the default, 0.5/s.");
    }

    if(ImGui::DragFloat("Motor speed cap (m/s)", &simulation.maximumMotorSpeed, 0.1f, 0.0f, AirSimulationSettings::MaximumMotorSpeedLimit,
           "%.1f"))
    {
        Clamp(simulation.maximumMotorSpeed, 0.0f, AirSimulationSettings::MaximumMotorSpeedLimit);
    }

    simulationDrag();
    ImGui::EndDisabled();

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

    // AIR.4F.2b.1: the existing fade, a true per-second rate (each fixed step scales mist by exp(-rate x step)), with finer steps and
    // presets for long-lived fog. R1.1: scene simulation configuration; independent of the air's damping and of emission.
    ImGui::BeginDisabled(!simulationEditable);

    if(ImGui::DragFloat("Mist Dissipation (1/s)", &simulation.mistDissipationRate, 0.001f, 0.0f, AirSimulationSettings::MaximumMistDissipationRate,
           "%.3f"))
    {
        Clamp(simulation.mistDissipationRate, 0.0f, AirSimulationSettings::MaximumMistDissipationRate);
    }

    simulationDrag();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How fast mist fades: its density shrinks by e^(-rate x seconds), so 1/rate is its lifetime (0: never fades).\n"
            "Mist exists only while an enabled Mist Emitter feeds it; it is carried by the simulated air and never crosses solid geometry.");
    }

    struct DissipationPreset
    {
        const char* label;
        float rate;
    };

    static constexpr DissipationPreset DissipationPresets[] = {{"0", 0.0f}, {"0.01", 0.01f}, {"0.05", 0.05f}, {"0.1", 0.1f}};

    for(const DissipationPreset& preset : DissipationPresets)
    {
        if(ImGui::SmallButton(preset.label))
        {
            simulation.mistDissipationRate = preset.rate;
            simulationCommit = true;
        }

        ImGui::SameLine();
    }

    if(ImGui::SmallButton("Default (0.25)"))
    {
        simulation.mistDissipationRate = AirSimulationSettings{}.mistDissipationRate;
        simulationCommit = true;
    }

    ImGui::EndDisabled();

    DrawVolumetricQualityControls(debugUI);

    // V1d follow-up, session only (never saved): mist and haze both draw through the chosen renderer.
    static constexpr const char* RendererNames[] = {"Original Raymarch", "Integrated Froxels (Experimental)"};
    int volumetricRenderer = std::clamp(air.volumetricRenderer, 0, 1);

    if(ImGui::Combo("Volumetric Renderer", &volumetricRenderer, RendererNames, IM_ARRAYSIZE(RendererNames)))
    {
        air.volumetricRenderer = volumetricRenderer;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Session only. Original Raymarch: the production renderer. Integrated Froxels: the V1d renderer (lighting\n"
            "integrated per froxel, Lights near their source marched per pixel), for comparison. Applies to lit mist and\n"
            "haze in the Editor and Play viewports; unlit mist always uses the raymarch.");
    }

    // Fog step 1, session only (never saved): the Original Raymarch's render volume.
    ImGui::BeginDisabled(air.volumetricRenderer != 0);
    ImGui::Checkbox("Mist Render Volume (Experimental)", &air.mistRenderVolume);

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Session only. The Original Raymarch reads open mist (away from geometry and the air window's edge) from a\n"
            "filterable copy built each frame, instead of gathering it from the air field. Same look; compare by toggling.");
    }

    // Fog step 2, session only (never saved): the hybrid froxel lighting (needs the render volume); step 2b: automatic by default.
    ImGui::BeginDisabled(!air.mistRenderVolume);
    static constexpr const char* FroxelLightModes[] = {"Auto (8+ Lights)", "Off", "On"};
    int froxelLightsMode = std::clamp(air.mistFroxelLightsMode, 0, 2);

    if(ImGui::Combo("Froxel Light Volume", &froxelLightsMode, FroxelLightModes, IM_ARRAYSIZE(FroxelLightModes)))
    {
        air.mistFroxelLightsMode = froxelLightsMode;
    }

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Session only. On: lit mist and haze take the scene Lights from a camera-aligned volume built each frame, except\n"
            "within 1.5-3 m of each Light, which is still lit per sample (faster with many Lights; far Lights are interpolated).\n"
            "Off: every Light per sample (exact). Auto: on while 8 or more Lights reach the fog, off again at 6 or fewer.");
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();


    // V1d distant banding (temporary): the medium's adaptive depth sampling against the fixed 4 samples per slice.
    ImGui::BeginDisabled(air.volumetricRenderer != 1);
    ImGui::Checkbox("Adaptive Depth Sampling (Diagnostic)", &air.volumetricAdaptiveSamples);
    // V1d lateral experiment (temporary).
    static constexpr const char* LateralNames[] = {"Baseline 1x1", "2x2 Lateral", "2x2 Cost Parity"};
    int lateralMode = std::clamp(air.volumetricLateralMode, 0, 2);

    if(ImGui::Combo("Lateral Sampling (Diagnostic)", &lateralMode, LateralNames, IM_ARRAYSIZE(LateralNames)))
    {
        air.volumetricLateralMode = lateralMode;
    }

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Temporary. Baseline: each froxel's medium from its centre ray, 4 depth samples. 2x2 Lateral: 4 distinct rays\n"
            "across the froxel, 4 depth samples each. 2x2 Cost Parity: 4 rays, 1 depth sample each. Compare with the Gaussian\n"
            "filter and Adaptive Depth Sampling off.");
    }

    ImGui::Checkbox("Double XY Resolution (Diagnostic)", &air.volumetricDoubleXY);

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Temporary. Froxel columns of 4 reduced pixels instead of 8 (4x the froxels and their memory).");
    }

    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Temporary. On: each froxel slice takes one AIR sample per cell of its depth (4 to 16), so distant, deep\n"
            "slices no longer skip cells. Off: a fixed 4 samples per slice.");
    }

    ImGui::EndDisabled();

    DrawAirEnvironmentControls(scene);
    DrawMistRenderingControls(scene);

    // AIR.4F F4.5.1b.1: the air's temperature (ΔT from the scene's ambient) as points over the whole window.
    ImGui::SeparatorText("Temperature");
    ImGui::Checkbox("Show temperature", &air.showTemperature);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A point for each fluid piece at fixed places throughout the air window, coloured by its temperature relative\n"
            "to the scene's ambient. Ambient air shows too; a view only, the simulation is unchanged.");
    }

    ImGui::BeginDisabled(!air.showTemperature);
    static constexpr const char* TemperatureSpacingNames[] = {"1 cell", "2 cells", "4 cells", "8 cells"};
    int temperatureSpacing = std::clamp(air.temperatureSpacingExponent, 0, 3);

    if(ImGui::Combo("Point spacing", &temperatureSpacing, TemperatureSpacingNames, IM_ARRAYSIZE(TemperatureSpacingNames)))
    {
        air.temperatureSpacingExponent = temperatureSpacing;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Distance between points along each axis. Every spacing covers the whole window; 1 cell draws up to two million\n"
            "points. Only the view's density changes, never the simulation's.");
    }

    if(ImGui::DragFloat("Temperature display range (K)", &air.temperatureDisplayRange, 1.0f, 1.0f, 1000.0f, "%.0f"))
    {
        Clamp(air.temperatureDisplayRange, 1.0f, 1000.0f);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The temperature difference from the ambient at which a point is fully red (warmer) or fully blue (colder);\n"
            "beyond it the colour stays saturated.");
    }

    ImGui::TextColored(ImVec4(0.1f, 0.35f, 1.0f, 1.0f), "Cold (-%.0f K)", air.temperatureDisplayRange);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.82f, 0.82f, 0.82f, 1.0f), "  Ambient  ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.12f, 0.06f, 1.0f), "Hot (+%.0f K)", air.temperatureDisplayRange);
    ImGui::EndDisabled();

    // AIR.4E.4: the experimental pressure projection (off by default). R1.1: scene simulation configuration.
    ImGui::SeparatorText("Pressure (experimental)");
    ImGui::BeginDisabled(!simulationEditable);
    simulationCheckbox("Enable pressure projection", simulation.pressureProjection);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Experimental. After each air step, removes the air's divergence with a pressure solve, so air pushed into walls,\n"
            "floors and ceilings turns along them instead of vanishing. Costs a few milliseconds of GPU time per step; off restores\n"
            "the previous behaviour exactly.");
    }

    // R1.5: moving-solid displacement (off by default). Scene configuration like the projection; per-body participation is the
    // Physics Inspector's AIR participation.
    simulationCheckbox("Moving-solid displacement", simulation.movingSolids);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Moving physics bodies push the air and its mist aside: Character Controllers, and Dynamic and Kinematic\n"
            "bodies whose AIR participation includes them (Automatic: at least one AIR cell, 0.5 m, in their longest collider\n"
            "dimension). Also switches the pressure solver to its warm path. During Play or a simulation a change applies at once,\n"
            "as a session override.");
    }

    ImGui::TextDisabled("Moving solids: %s", simulation.movingSolids ? "displace the air" : "off (bodies do not affect the air)");

    // AIR.4F.1b: temperature-driven buoyancy (off by default). It acts through the projection, which it never turns on.
    simulationCheckbox("Enable buoyancy", simulation.buoyancy);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Experimental. Air warmer than the ambient rises and colder air sinks, from the air's temperature (Mist\n"
            "Emitters' Temperature heats or cools it; a Rate of 0 heats without adding mist). Applied by the pressure projection,\n"
            "so it needs \"Enable pressure projection\".");
    }

    ImGui::BeginDisabled(!simulation.buoyancy);
    ImGui::SetNextItemWidth(160.0f);

    if(ImGui::DragFloat("Gravity (m/s^2)", &simulation.buoyancyGravity, 0.05f, 0.0f, AirSimulationSettings::MaximumBuoyancyGravity, "%.2f",
           ImGuiSliderFlags_AlwaysClamp))
    {
        Clamp(simulation.buoyancyGravity, 0.0f, AirSimulationSettings::MaximumBuoyancyGravity);
    }

    simulationDrag();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The magnitude of gravity, acting straight down. 9.81 is Earth's; other values are for diagnostics and are\n"
            "not physical. 0 turns buoyancy off.");
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // The effective state, so a buoyancy that cannot act is never mistaken for one that does.
    if(!simulation.buoyancy)
    {
        ImGui::TextDisabled("Buoyancy: off");
    }
    else if(!simulation.pressureProjection)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Buoyancy: inactive (needs pressure projection)");
    }
    else if(simulation.buoyancyGravity <= 0.0f)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Buoyancy: inactive (gravity 0)");
    }
    else
    {
        ImGui::TextDisabled("Buoyancy: active, %.2f m/s^2 down, from the scene's ambient temperature", simulation.buoyancyGravity);
    }

    // AIR.4E.2: each fluid piece's divergence (net outflow through its open faces) as points over the whole window.
    ImGui::SeparatorText("Divergence");
    ImGui::Checkbox("Show divergence", &air.showDivergence);

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A point for each fluid piece at fixed places throughout the air window, coloured by how much more air flows\n"
            "out of it than in through its open faces (a pressure projection would drive this to zero). Computed only while shown;\n"
            "a view only, the simulation is unchanged.");
    }

    ImGui::BeginDisabled(!air.showDivergence);
    static constexpr const char* DivergenceSpacingNames[] = {"1 cell", "2 cells", "4 cells", "8 cells"};
    int divergenceSpacing = std::clamp(air.divergenceSpacingExponent, 0, 3);

    if(ImGui::Combo("Divergence point spacing", &divergenceSpacing, DivergenceSpacingNames, IM_ARRAYSIZE(DivergenceSpacingNames)))
    {
        air.divergenceSpacingExponent = divergenceSpacing;
    }

    static constexpr const char* MeasureNames[] = {"Face fluxes (after projection)", "Stored velocities (collocated)", "Before projection"};
    int measure = std::clamp(air.divergenceMeasure, 0, 2);

    if(ImGui::Combo("Measure", &measure, MeasureNames, IM_ARRAYSIZE(MeasureNames)))
    {
        air.divergenceMeasure = measure;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Face fluxes: with pressure projection on, the divergence the corrected face fluxes are predicted to have (near\n"
            "zero); off, the stored velocity's.\n"
            "Stored velocities: the per-piece velocities the solver and the mist actually carry. After a projection they keep\n"
            "some divergence: the correction is approximate for cell-centred velocity.\n"
            "Before projection: the step's divergence before its correction (projection on; otherwise the stored velocity's).");
    }

    if(ImGui::DragFloat("Divergence display range (1/s)", &air.divergenceDisplayRange, 0.01f, 0.01f, 100.0f, "%.2f"))
    {
        Clamp(air.divergenceDisplayRange, 0.01f, 100.0f);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The divergence (net outflow per second, relative to a whole cell's volume) at which a point is fully orange\n"
            "(air leaving: a source) or fully cyan (air arriving: a sink); beyond it the colour stays saturated.");
    }

    ImGui::TextColored(ImVec4(0.0f, 0.75f, 0.85f, 1.0f), "Sink (-%.2f/s)", air.divergenceDisplayRange);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.82f, 0.82f, 0.82f, 1.0f), "  Zero  ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Source (+%.2f/s)", air.divergenceDisplayRange);
    ImGui::EndDisabled();

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

    if(simulationEditable && (simulation != displayed || simulationCommit))
    {
        ApplyAirSimulationEdit(scene, simulation, simulationCommit);
    }
}

// R1.3: the project's Volumetrics settings (Project Settings) as this frame renders them. A change here is a session override in the
// Debug panel's RenderingOverrides, never saved; "Use project setting" clears it. They apply to the haze and the AIR mist alike.
void Editor::DrawVolumetricQualityControls(DebugUI& debugUI)
{
    const VolumetricRenderingSettings effective = debugUI.GetEffectiveRenderingSettings().volumetrics;
    RenderingOverrides& overrides = debugUI.GetRenderingOverrides();
    const auto overrideNote = [](const char* id, auto& slot)
    {
        if(!slot)
        {
            return;
        }

        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Session override (not saved)");
        ImGui::SameLine();

        if(ImGui::SmallButton(id))
        {
            slot.reset();
        }
    };

    static constexpr const char* QualityNames[] = {"Low (1/4 res, 32 steps)", "Medium (1/4 res, 64 steps)", "High (1/2 res, 96 steps)"};
    int quality = std::clamp(static_cast<int>(effective.quality), 0, 2);

    if(ImGui::Combo("Volumetric quality", &quality, QualityNames, IM_ARRAYSIZE(QualityNames)))
    {
        overrides.volumetricQuality = static_cast<ProjectVolumetricQuality>(quality);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The project's setting (Project Settings, Volumetrics). A change here overrides it for this session only.");
    }

    overrideNote("Use project setting##VolumetricQuality", overrides.volumetricQuality);

    static constexpr const char* BasisNames[] = {"Internal Resolution", "Output Resolution"};
    int basis = std::clamp(static_cast<int>(effective.resolutionBasis), 0, 1);

    if(ImGui::Combo("Volumetric resolution basis", &basis, BasisNames, IM_ARRAYSIZE(BasisNames)))
    {
        overrides.volumetricResolutionBasis = static_cast<ProjectVolumetricResolutionBasis>(basis);
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("What the quality's resolution scales: the scene's render resolution (lower under DLSS or a render scale) or\n"
            "the output image. The fog is always composited at the output. A change here overrides the project for this session.");
    }

    overrideNote("Use project setting##VolumetricBasis", overrides.volumetricResolutionBasis);
}

std::optional<AirSimulationSettings> Editor::GetAirSimulationOverride(std::uint64_t runtimeIdentity) const
{
    if(!IsSceneEditingLocked() || !m_airSimulationOverride || m_airSimulationOverride->runtimeIdentity != runtimeIdentity)
    {
        return std::nullopt;
    }

    return m_airSimulationOverride->settings;
}

void Editor::DiscardAirSimulationOverride() noexcept
{
    m_airSimulationOverride.reset();
}

AirSimulationSettings Editor::GetDisplayedAirSimulation(IDemoScene& scene) const
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(owner == nullptr || !owner->m_runtime)
    {
        return AirSimulationSettings{};
    }

    const std::uint64_t runtimeIdentity = owner->GetRuntimeIdentity();
    const SceneDocument* document = !owner->IsArchiveActivation() ? &owner->m_runtime->GetFileScene().GetDocument() : nullptr;

    if(IsSceneEditingLocked())
    {
        return ResolveAirSimulation(document, GetAirSimulationOverride(runtimeIdentity));
    }

    if(m_airSimulationDraft && m_airSimulationDraft->runtimeIdentity == runtimeIdentity && document != nullptr)
    {
        return m_airSimulationDraft->settings;
    }

    return ResolveAirSimulation(document, std::nullopt);
}

// While Play or a simulation locks the scene, every edit is the session override, live and unsaved, never history. In a stopped scene
// an edit is a scene edit: a drag's intermediate values only update the draft (the simulation takes the value on release), and a
// finished edit is one history entry.
void Editor::ApplyAirSimulationEdit(IDemoScene& scene, const AirSimulationSettings& edited, bool commit)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(owner == nullptr || !owner->m_runtime)
    {
        return;
    }

    const std::uint64_t runtimeIdentity = owner->GetRuntimeIdentity();

    if(IsSceneEditingLocked())
    {
        m_airSimulationOverride = AirSimulationSessionValue{runtimeIdentity, edited};
        m_airSimulationDraft.reset();
        m_airSimulationStatus.clear();
        return;
    }

    if(owner->IsArchiveActivation())
    {
        return;
    }

    if(!commit)
    {
        m_airSimulationDraft = AirSimulationSessionValue{runtimeIdentity, edited};
        return;
    }

    m_airSimulationDraft.reset();
    const std::optional<AirSimulationSettings> stored = owner->m_runtime->GetFileScene().GetDocument().airSimulation;
    std::string error;
    m_airSimulationStatus = CommitAirSimulation(scene, stored, edited, &error) ? std::string{} : error;
}

size_t Editor::AirSimulationEditPayloadBytes(const AirSimulationEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(AirSimulationEdit) + edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath);
}

// R1.1: one history entry for the scene's AIR simulation configuration, as CommitAirEnvironment: the first edit of a scene without the
// block stores it (from its defaults); undo returns the scene to having none.
bool Editor::CommitAirSimulation(IDemoScene& scene, const std::optional<AirSimulationSettings>& before, const AirSimulationSettings& after,
    std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "The AIR simulation settings can be changed in a stopped scene without pending edits.";
        }

        return false;
    }

    if(before.value_or(AirSimulationSettings{}) == after)
    {
        return true;
    }

    if(!ValidateAirSimulationSettings(after, error))
    {
        return false;
    }

    try
    {
        auto command = std::make_unique<AirSimulationEdit>(AirSimulationEdit{*m_historySession, before, after});
        const size_t payload = AirSimulationEditPayloadBytes(*command);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneAirSimulation(before, after, error))
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

bool Editor::ReplayAirSimulation(IDemoScene& scene, const AirSimulationEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "AIR simulation replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneAirSimulation(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

size_t Editor::AirEnvironmentEditPayloadBytes(const AirEnvironmentEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(AirEnvironmentEdit) + edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath);
}

// One history entry for the scene's air environment. The first edit of a scene without the block stores it (from its defaults); undo
// returns the scene to having none.
bool Editor::CommitAirEnvironment(IDemoScene& scene, const std::optional<AirEnvironmentSettings>& before, const AirEnvironmentSettings& after,
    std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "The air environment can be changed in a stopped scene without pending edits.";
        }

        return false;
    }

    if(before.value_or(AirEnvironmentSettings{}) == after)
    {
        return true;
    }

    if(!ValidateAirEnvironmentSettings(after, error))
    {
        return false;
    }

    try
    {
        auto command = std::make_unique<AirEnvironmentEdit>(AirEnvironmentEdit{*m_historySession, before, after});
        const size_t payload = AirEnvironmentEditPayloadBytes(*command);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneAirEnvironment(before, after, error))
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

bool Editor::ReplayAirEnvironment(IDemoScene& scene, const AirEnvironmentEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Air environment replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneAirEnvironment(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

// AIR.4F F4.5.1b: the scene's air environment. Each field commits when its input is finished (Enter or leaving the field), as one
// history entry; an invalid value is refused with a message and the scene keeps its value.
void Editor::DrawAirEnvironmentControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner && owner->m_runtime && !owner->IsArchiveActivation() ? &owner->m_runtime->GetFileScene().GetDocument() :
        nullptr;
    ImGui::SeparatorText("Environment (scene)");

    if(document == nullptr)
    {
        ImGui::TextDisabled("Open a project scene to edit the air environment.");
        return;
    }

    if(IsEnvironmentGroupInherited(scene, static_cast<int>(FileBackedDemoScene::EnvironmentGroup::AirEnvironment)))
    {
        ImGui::TextDisabled("Inherited. Enable the AIR environment override (Environment, Local Level) to edit this Local Level's own.");
        return;
    }

    const std::optional<AirEnvironmentSettings> stored = document->airEnvironment;
    const AirEnvironmentSettings current = stored.value_or(AirEnvironmentSettings{});
    AirEnvironmentSettings edited = current;
    const bool editable = !IsSceneEditingLocked() && m_gameState == GameState::Editing;
    bool commit = false;
    ImGui::BeginDisabled(!editable);
    ImGui::InputFloat("Ambient temperature (K)", &edited.ambientTemperature, 1.0f, 10.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The air's temperature where nothing heats or cools it, in kelvin (%.2f degrees Celsius now; 223.15 to 373.15 K).\n"
            "Air temperature is stored relative to it, so changing it shifts every temperature in the field with it.",
            current.ambientTemperature - 273.15f);
    }

    ImGui::InputFloat("Thermal relaxation time (s)", &edited.relaxationTime, 1.0f, 10.0f, "%.1f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How quickly heated or cooled air returns to the ambient: its difference falls to about 37%% in this time\n"
            "(an exponential time constant). 0 turns it off; at most 3600 s.");
    }

    ImGui::Text("Ambient: %.2f degrees Celsius", current.ambientTemperature - 273.15f);
    ImGui::EndDisabled();

    if(!m_airEnvironmentStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_airEnvironmentStatus.c_str());
    }

    if(!editable || !commit)
    {
        return;
    }

    std::string error;
    m_airEnvironmentStatus = CommitAirEnvironment(scene, stored, edited, &error) ? std::string{} : error;
}

size_t Editor::EnvironmentalHazeEditPayloadBytes(const EnvironmentalHazeEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(EnvironmentalHazeEdit) + edit.session.sceneIdentity.capacity() +
        pathBytes(edit.session.projectIdentity) + pathBytes(edit.session.scenePath);
}

// AIR.4F.2d.3: one history entry for the scene's environmental haze, as CommitAirEnvironment: the first edit of a scene without the block
// stores it; undo returns the scene to having none.
bool Editor::CommitEnvironmentalHaze(IDemoScene& scene, const std::optional<EnvironmentalHazeSettings>& before,
    const EnvironmentalHazeSettings& after, std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "The environmental haze can be changed in a stopped scene without pending edits.";
        }

        return false;
    }

    if(before.value_or(EnvironmentalHazeSettings{}) == after)
    {
        return true;
    }

    if(!ValidateEnvironmentalHazeSettings(after, error))
    {
        return false;
    }

    try
    {
        auto command = std::make_unique<EnvironmentalHazeEdit>(EnvironmentalHazeEdit{*m_historySession, before, after});
        const size_t payload = EnvironmentalHazeEditPayloadBytes(*command);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneEnvironmentalHaze(before, after, error))
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

bool Editor::ReplayEnvironmentalHaze(IDemoScene& scene, const EnvironmentalHazeEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Environmental haze replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneEnvironmentalHaze(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

// AIR.4F.2d.3: the Scene Inspector's Environmental Haze section. Each field commits when its input is finished, as one history entry;
// an invalid value is refused with a message and the scene keeps its value.
void Editor::DrawEnvironmentalHazeControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner && owner->m_runtime && !owner->IsArchiveActivation() ? &owner->m_runtime->GetFileScene().GetDocument() :
        nullptr;

    if(!ImGui::CollapsingHeader("Environmental Haze"))
    {
        return;
    }

    if(document == nullptr)
    {
        ImGui::TextDisabled("Open a project scene to edit its environmental haze.");
        return;
    }

    if(IsEnvironmentGroupInherited(scene, static_cast<int>(FileBackedDemoScene::EnvironmentGroup::Haze)))
    {
        ImGui::TextDisabled("Inherited. Enable the Haze override (Environment, Local Level) to edit this Local Level's own.");
        return;
    }

    const std::optional<EnvironmentalHazeSettings> stored = document->environmentalHaze;
    EnvironmentalHazeSettings edited = stored.value_or(EnvironmentalHazeSettings{});
    const bool editable = !IsSceneEditingLocked() && m_gameState == GameState::Editing;
    bool commit = false;
    ImGui::PushID("EnvironmentalHaze");
    ImGui::BeginDisabled(!editable);

    if(ImGui::Checkbox("Enabled", &edited.enabled))
    {
        commit = true;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A uniform atmosphere filling the whole world, beyond the air window, up to geometry and the sky. It uses the\n"
            "mist's sun, scattering and phase settings (Air window), with its own ambient contribution. Geometry does not shadow it yet (the simulated mist is shadowed). Rendered with\n"
            "the Editor's fog, so packaged games do not show it yet.");
    }

    ImGui::BeginDisabled(!edited.enabled);
    ImGui::InputFloat("Fog Attenuation Distance (m)", &edited.attenuationDistance, 1.0f, 10.0f, "%.1f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How far you see through the haze alone: over this distance it lets 37%% (1/e) of the background through, over\n"
            "twice it 14%%, three times 5%%. Its extinction is 1 / distance; the sky lies at the camera's far distance.");
    }

    ImGui::ColorEdit3("Haze Color", edited.color.data(), ImGuiColorEditFlags_Float);
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The haze's scattering colour (its albedo per channel): what it scatters of the sun's, the Lights' and the\n"
            "environment's light, multiplied by the scene's scattering tint and strength. What it does not scatter it absorbs, so it\n"
            "never glows.");
    }

    ImGui::SliderFloat("Ambient Contribution", &edited.ambient, 0.0f, EnvironmentalHazeSettings::MaximumAmbient, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The environment light (the sky the scene is lit by) that the haze scatters, independently of the simulated\n"
            "mist's Ambient. 1 lights it with the whole environment unoccluded, so distant objects fade toward the sky's brightness;\n"
            "0 leaves only the sun and Lights, which makes haze darker than the sky. Above 1 adds light the scene does not have.");
    }

    ImGui::EndDisabled();
    ImGui::InputFloat("Haze Near Distance (m)", &edited.nearDistance, 0.5f, 5.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Keeps the camera's surroundings readable: this haze is clear up to this distance and reaches full strength\n"
            "over the transition beyond it. The simulated AIR mist is not affected, so nearby AIR fog stays visible (it has its own\n"
            "AIR Near Distance in the Air window). 0 turns it off.");
    }

    ImGui::BeginDisabled(edited.nearDistance <= 0.0f);
    ImGui::InputFloat("Haze Near Transition (m)", &edited.nearTransition, 0.5f, 5.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The distance over which the haze fades in beyond the Haze Near Distance (a smooth curve). Wider is softer;\n"
            "below about a metre the edge of the clear region can show.");
    }

    ImGui::EndDisabled();

    // AIR.4F.2d.3b: world-space height bounds.
    if(ImGui::Checkbox("Enable Height Bounds", &edited.heightBounds))
    {
        commit = true;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Confines the haze to a horizontal layer between two world heights (Y), fading out over the Height Transition\n"
            "below the base and above the top. The simulated AIR mist is not affected.");
    }

    ImGui::BeginDisabled(!edited.heightBounds);
    ImGui::InputFloat("Base Height (m)", &edited.baseHeight, 0.5f, 5.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::InputFloat("Max Height (m)", &edited.maxHeight, 0.5f, 5.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("World heights (Y) of the layer's bottom and top: the haze is at full density between them. The top must be above\n"
            "the base.");
    }

    ImGui::InputFloat("Height Transition (m)", &edited.heightTransition, 0.5f, 5.0f, "%.2f");
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How far the haze fades out below the base and above the top (a smooth curve, outside the layer, so a thin\n"
            "layer keeps its full density). 0 is a hard edge.");
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if(!m_environmentalHazeStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_environmentalHazeStatus.c_str());
    }

    ImGui::PopID();

    if(!editable || !commit)
    {
        return;
    }

    std::string error;
    m_environmentalHazeStatus = CommitEnvironmentalHaze(scene, stored, edited, &error) ? std::string{} : error;
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
// Every MistRenderSettings field with its tooltip. A checkbox is a whole edit; a slider is one while it is held.
MistFieldEdit DrawMistAppearanceFields(MistRenderSettings& settings)
{
    bool changed = false;
    bool commit = false;
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

    // AIR.4F.2d.2: the scattering colour, and the multiple-scattering approximation (both neutral by default).
    slider(ImGui::ColorEdit3("Scattering tint", settings.scatteringTint.data(), ImGuiColorEditFlags_Float));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The colour of the light the mist scatters, multiplying each emitter's colour. It never makes the mist more\n"
            "opaque or brighter than white: what a channel does not scatter, the mist absorbs. White changes nothing.");
    }

    slider(ImGui::SliderFloat("Multiple scattering", &settings.multipleScattering, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Deep inside thick, bright mist the sun's light has scattered many times and lost its direction: this\n"
            "spreads the sun's scattered light more evenly over viewing directions the deeper a ray goes into the mist, so dense mist\n"
            "away from the sun is less dark (and toward it less glaring). It adds no light: the total scattered is unchanged, shadows\n"
            "stay, and thin mist is unaffected. Unlike Ambient contribution it follows the sun. 0 is single scattering only.");
    }

    // AIR.4F.2d.3e: sunlight carried into shadowed mist by the sunlit mist around it (it needs the sun's shadows).
    ImGui::BeginDisabled(!settings.sunShadows);
    slider(ImGui::SliderFloat("Indirect scattering", &settings.indirectScattering, 0.0f, 1.0f, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Lets sunlit mist light the shadowed mist near it (within about 8 m), as fog does: the light the sunlit mist\n"
            "scatters reaches the shadow, weakened by the mist it crosses, and never passes through walls, floors or other\n"
            "geometry. It only brightens the shadowed part of the mist, so sunlit mist and the shadows' shape are unchanged; thin mist\n"
            "carries little. 1 is the full estimate; 0 is off. Unlike Ambient contribution it follows the sun and the shadows.");
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // AIR.4F.2d.3b: the AIR mist's own near clearance (the environmental haze has its own, in the Scene Inspector).
    slider(ImGui::DragFloat("AIR Near Distance (m)", &settings.nearDistance, 0.05f, 0.0f, MistRenderSettings::MaximumNearDistance, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Clears the simulated AIR mist around the camera: none within this distance, full strength past the\n"
            "transition beyond it. The environmental haze is not affected (it has its own Haze Near Distance in the Scene\n"
            "Inspector). The mist itself is unchanged, only drawn thinner near the camera. 0 turns it off.");
    }

    ImGui::BeginDisabled(settings.nearDistance <= 0.0f);
    slider(ImGui::DragFloat("AIR Near Transition (m)", &settings.nearTransition, 0.05f, MistRenderSettings::MinimumNearTransition,
        MistRenderSettings::MaximumNearTransition, "%.2f"));

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The distance over which the AIR mist fades in beyond its near distance (a smooth curve). Below about a\n"
            "metre the edge of the clear region can show.");
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

    return {changed, commit};
}

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

    if(IsEnvironmentGroupInherited(scene, static_cast<int>(FileBackedDemoScene::EnvironmentGroup::Mist)))
    {
        ImGui::TextDisabled("Inherited. Enable the Mist appearance override (Environment, Local Level) to edit this Local Level's own.");
        return;
    }

    const std::optional<MistRenderSettings> stored = document->mistRendering;
    MistRenderSettings settings = GetEffectiveMistRendering(document, runtimeIdentity);
    const bool editable = !IsSceneEditingLocked() && m_gameState == GameState::Editing;
    ImGui::BeginDisabled(!editable);
    const MistFieldEdit edit = DrawMistAppearanceFields(settings);
    const bool changed = edit.changed;
    const bool commit = edit.commit;
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
