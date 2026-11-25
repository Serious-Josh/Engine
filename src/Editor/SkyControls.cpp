// Physical sky S1 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.12): the Level > Sky window. HDRP's Visual
// Environment and Physically Based Sky sections and field names, for the open document's Sky environment group. The window edits
// the stored physical values only: HDRP's density and tint are exact views of the coefficients (density = sigma_t * H, the optical
// depth at the zenith; tint = sigma_s / sigma_t), and the collapsed Physical Coefficients section shows the same values in
// Unreal's form (10^-6 m^-1, km). Each completed edit is one SkyEdit history entry; a held drag re-bakes live from a draft.
// Editable only while stopped, like the environmental haze. Invalid values are refused and the stored value kept, never clamped.
#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Editor/InspectorNumericDrag.h"
#include "Renderer/Renderer.h"
#include "Platform/Windows/WindowsWindow.h"
#include "Scene/CompositeWorld.h"
#include "Scene/SceneLighting.h"
#include "Scene/SunDirection.h"

#include <imgui.h>

#include <array>
#include <string>

namespace
{
    using Group = FileBackedDemoScene::EnvironmentGroup;

    // The window lists the sky types in HDRP's order.
    constexpr SkyType SkyTypeOrder[] = {SkyType::Physical, SkyType::Hdri, SkyType::Neutral};

    int SkyTypeIndex(SkyType type)
    {
        for(int index = 0; index < 3; ++index)
        {
            if(SkyTypeOrder[index] == type)
            {
                return index;
            }
        }

        return 2;
    }

    // What this frame's fields did: a drag or typed entry is held, completed or cancelled.
    struct SkyFieldResults
    {
        bool editing = false;
        bool committed = false;
        bool cancelled = false;
    };

    void Track(SkyFieldResults& results, InspectorDragResult result)
    {
        switch(result)
        {
        case InspectorDragResult::Editing:
            results.editing = true;
            break;
        case InspectorDragResult::Committed:
            results.committed = true;
            break;
        case InspectorDragResult::Cancelled:
            results.cancelled = true;
            break;
        case InspectorDragResult::Idle:
        default:
            break;
        }
    }

    void Tooltip(const char* text)
    {
        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    constexpr InspectorDragSpec PlanetRadiusDrag{1.0f, PhysicalSkySettings::MinimumPlanetRadiusKm,
        PhysicalSkySettings::MaximumPlanetRadiusKm, false, "%.1f"};
    constexpr InspectorDragSpec SeaLevelDrag{0.1f, -PhysicalSkySettings::MaximumSeaLevelMeters,
        PhysicalSkySettings::MaximumSeaLevelMeters, false, "%.2f"};
    constexpr InspectorDragSpec UnitDrag{0.005f, 0.0f, 1.0f, false, "%.3f"};
    constexpr InspectorDragSpec DensityDrag{0.0005f, 0.0f, 80.0f, false, "%.4f"};
    constexpr InspectorDragSpec ScaleHeightDrag{0.01f, PhysicalSkySettings::MinimumScaleHeightKm,
        PhysicalSkySettings::MaximumScaleHeightKm, false, "%.3f"};
    constexpr InspectorDragSpec AnisotropyDrag{0.005f, -PhysicalSkySettings::MaximumAnisotropy,
        PhysicalSkySettings::MaximumAnisotropy, false, "%.3f"};
    constexpr InspectorDragSpec OzoneDimmerDrag{0.01f, 0.0f, 10.0f, false, "%.3f"};
    constexpr InspectorDragSpec OzoneAltitudeDrag{0.1f, 0.0f, PhysicalSkySettings::MaximumOzoneAltitudeKm, false, "%.2f"};
    constexpr InspectorDragSpec CoefficientDrag{0.01f, 0.0f, 10000.0f, false, "%.4f"};
    constexpr InspectorDragSpec ShiftDrag{0.005f, -1.0f, 1.0f, false, "%.3f"};
    constexpr InspectorDragSpec ExposureDrag{0.05f, -PhysicalSkySettings::MaximumExposureCompensation,
        PhysicalSkySettings::MaximumExposureCompensation, false, "%.2f"};
    constexpr InspectorDragSpec MultiplierDrag{0.01f, 0.0f, PhysicalSkySettings::MaximumMultiplier, false, "%.3f"};
    constexpr InspectorDragSpec MultipleScatteringDrag{0.01f, 0.0f, PhysicalSkySettings::MaximumMultipleScattering, false, "%.2f"};
    constexpr InspectorDragSpec PeriodDrag{0.05f, 0.0f, PhysicalSkySettings::MaximumUpdatePeriodSeconds, false, "%.2f"};
    // S5: the night sky and the clouds.
    constexpr InspectorDragSpec NightIntensityDrag{0.01f, 0.0f, PhysicalSkySettings::MaximumNightSkyIntensity, false, "%.3f"};
    constexpr InspectorDragSpec AirglowDrag{0.00005f, 0.0f, PhysicalSkySettings::MaximumAirglowLuminance, false, "%.5f"};
    constexpr InspectorDragSpec RotationDrag{0.25f, -PhysicalSkySettings::MaximumPlanetRotationDegrees,
        PhysicalSkySettings::MaximumPlanetRotationDegrees, false, "%.2f"};
    constexpr InspectorDragSpec CloudAltitudeDrag{0.05f, 0.0f, CloudLayerSettings::MaximumAltitudeKm, false, "%.2f"};
    constexpr InspectorDragSpec CloudThicknessDrag{0.05f, CloudLayerSettings::MinimumThicknessKm, CloudLayerSettings::MaximumThicknessKm,
        false, "%.2f"};
    constexpr InspectorDragSpec CloudDensityDrag{0.1f, 0.0f, CloudLayerSettings::MaximumDensity, false, "%.2f"};
    constexpr InspectorDragSpec CloudShapeScaleDrag{0.1f, CloudLayerSettings::MinimumShapeScaleKm, CloudLayerSettings::MaximumShapeScaleKm,
        false, "%.2f"};
    constexpr InspectorDragSpec CloudDetailScaleDrag{0.01f, CloudLayerSettings::MinimumDetailScaleKm,
        CloudLayerSettings::MaximumDetailScaleKm, false, "%.3f"};
    constexpr InspectorDragSpec CloudForwardDrag{0.005f, 0.0f, CloudLayerSettings::MaximumPhase, false, "%.3f"};
    constexpr InspectorDragSpec CloudBackwardDrag{0.005f, -CloudLayerSettings::MaximumPhase, 0.0f, false, "%.3f"};
    constexpr InspectorDragSpec CloudAmbientDrag{0.01f, 0.0f, CloudLayerSettings::MaximumAmbientStrength, false, "%.2f"};
    constexpr InspectorDragSpec WindDirectionDrag{0.5f, -360.0f, 360.0f, false, "%.1f"};
    constexpr InspectorDragSpec WindSpeedDrag{0.1f, 0.0f, CloudLayerSettings::MaximumWindSpeed, false, "%.1f"};
    constexpr InspectorDragSpec AtmosphereHeightDrag{0.5f, PhysicalSkySettings::MinimumAtmosphereHeightKm,
        PhysicalSkySettings::MaximumAtmosphereHeightKm, false, "%.1f"};

    // HDRP's Density and Tint of one medium (§4.12.3). The coefficients change only when the user changes the view, so an untouched
    // view never rounds the stored values.
    void DrawDensityAndTint(SkyFieldResults& results, std::array<float, 3>& scattering, std::array<float, 3>& absorption,
        float scaleHeightKm, const char* densityTooltip)
    {
        float density[3]{};
        float tint[3]{};

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            density[channel] = float(PhysicalSkyZenithDensity(scattering[channel], absorption[channel], scaleHeightKm));
            tint[channel] = float(PhysicalSkyTint(scattering[channel], absorption[channel]));
        }

        const std::array<float, 3> shownDensity{density[0], density[1], density[2]};
        const std::array<float, 3> shownTint{tint[0], tint[1], tint[2]};
        Track(results, InspectorDragFloats("Density", density, 3, DensityDrag));
        Tooltip(densityTooltip);
        Track(results, InspectorDragFloats("Tint", tint, 3, UnitDrag));
        Tooltip("Single-scattering albedo per channel (sigma_s / sigma_t): 1 scatters everything it extinguishes, 0 absorbs it.");

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            if(density[channel] != shownDensity[channel] || tint[channel] != shownTint[channel])
            {
                PhysicalSkyCoefficientsFromView(density[channel], tint[channel], scaleHeightKm, scattering[channel],
                    absorption[channel]);
            }
        }
    }

    // A per-metre coefficient triple shown in 10^-6 m^-1 (the Physical Coefficients view).
    void DrawMicroCoefficients(SkyFieldResults& results, const char* label, std::array<float, 3>& coefficients)
    {
        float micro[3]{};

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            micro[channel] = float(double(coefficients[channel]) * 1.0e6);
        }

        const std::array<float, 3> shown{micro[0], micro[1], micro[2]};
        Track(results, InspectorDragFloats(label, micro, 3, CoefficientDrag));

        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            if(micro[channel] != shown[channel])
            {
                coefficients[channel] = float(double(micro[channel]) * 1.0e-6);
            }
        }
    }
}

size_t Editor::SkyEditPayloadBytes(const SkyEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
    return sizeof(SceneHistoryEntry) + sizeof(SkyEdit) + edit.session.sceneIdentity.capacity() +
        pathBytes(edit.session.projectIdentity) + pathBytes(edit.session.scenePath);
}

// One history entry for the open document's sky type and physical sky, as CommitEnvironmentalHaze: the first physical edit of a
// document without a stored physical sky stores it; undo returns the document to having none.
bool Editor::CommitSky(IDemoScene& scene, const SceneSky& before, const SceneSky& after, std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || owner->IsArchiveActivation() || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() || m_sceneOperationPending ||
        HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "The sky can be changed in a stopped scene without pending edits.";
        }

        return false;
    }

    if(before == after)
    {
        return true;
    }

    if(after.type && !IsValidSkyType(*after.type))
    {
        if(error)
        {
            *error = "Invalid sky type.";
        }

        return false;
    }

    if(after.physical && !ValidatePhysicalSkySettings(*after.physical, error))
    {
        return false;
    }

    try
    {
        auto command = std::make_unique<SkyEdit>(SkyEdit{*m_historySession, before, after});
        const size_t payload = SkyEditPayloadBytes(*command);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetSceneSky(before, after, error))
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

bool Editor::ReplaySky(IDemoScene& scene, const SkyEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Sky replay belongs to a stale scene session.";
        return false;
    }

    return owner->SetSceneSky(redo ? edit.before : edit.after, redo ? edit.after : edit.before, &error);
}

std::optional<PhysicalSkySettings> Editor::GetRenderedPhysicalSky(const FileBackedDemoScene& scene) const
{
    std::optional<PhysicalSkySettings> effective = scene.GetEffectivePhysicalSky();

    if(effective && m_physicalSkyDraft && m_physicalSkyDraft->runtimeIdentity == scene.GetRuntimeIdentity())
    {
        return m_physicalSkyDraft->settings;
    }

    return effective;
}

void Editor::DrawSkySunSection(IDemoScene& scene, FileBackedDemoScene& owner)
{
    if(!ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // The sun is the scene's shadow owner: its first enabled Directional Light.
    const SceneObjectDocument* sun = FindDirectionalShadowOwner(owner.m_runtime->GetFileScene().GetDocument());
    const auto metadata = sun ? owner.GetObjectMetadata(sun->id) : std::nullopt;

    if(!sun || !sun->light || !metadata)
    {
        ImGui::TextWrapped("No enabled Directional Light: the sky has no sun and stays dark. Add a Directional Light.");
        return;
    }

    ImGui::Text("Sun: %s", sun->name.c_str());

    if(!sun->parentID.empty())
    {
        ImGui::TextWrapped("The sun has a parent; set its direction with its transform in the Inspector.");
        return;
    }

    const bool editable = !IsSceneEditingLocked() && m_gameState == GameState::Editing && !m_sceneOperationPending;
    const RenderObjectID index = metadata->runtimeIndex;
    const Transform current = static_cast<const Scene&>(scene.GetScene()).GetRenderObjects()[index].transform;

    if(!m_skySunEditing)
    {
        const SunAngles angles = SunDirection::AnglesFromTravel(SunDirection::TravelFromRotation(current.rotation));
        m_skySunDrafts = {angles.elevationDegrees, angles.azimuthDegrees};
    }

    ImGui::BeginDisabled(!editable);
    constexpr InspectorDragSpec ElevationDrag{0.1f, -90.0f, 90.0f, false, "%.2f"};
    constexpr InspectorDragSpec AzimuthDrag{0.25f, 0.0f, 360.0f, false, "%.2f"};
    const InspectorDragResult elevation = InspectorDragFloats("Elevation (deg)", &m_skySunDrafts[0], 1, ElevationDrag);
    Tooltip("The sun's height above the horizon. Below 0 the planet hides it and the sky darkens into twilight.");
    const InspectorDragResult azimuth = InspectorDragFloats("Azimuth (deg)", &m_skySunDrafts[1], 1, AzimuthDrag);
    Tooltip("The direction toward the sun, clockwise from +Z toward +X.");

    bool affectsAtmosphere = sun->light->affectsAtmosphere;

    if(ImGui::Checkbox("Affect Physically Based Sky", &affectsAtmosphere) && editable)
    {
        LightComponent edited = *sun->light;
        edited.affectsAtmosphere = affectsAtmosphere;
        CommitObjectLight(scene, *metadata, sun->light, edited);
    }

    ImGui::Text("Illuminance above the atmosphere: %.0f lux", sun->light->intensity);
    ImGui::EndDisabled();

    if(!editable)
    {
        return;
    }

    const auto isEditing = [](InspectorDragResult result)
    {
        return result == InspectorDragResult::Editing;
    };
    const bool editing = isEditing(elevation) || isEditing(azimuth);
    const bool committed = elevation == InspectorDragResult::Committed || azimuth == InspectorDragResult::Committed;
    const bool cancelled = elevation == InspectorDragResult::Cancelled || azimuth == InspectorDragResult::Cancelled;

    if((editing || committed) && !m_skySunEditing)
    {
        // A transform history entry spans the whole gesture, as the Inspector's transform fields do.
        m_skySunEditing = true;
        m_skySunUndoStart = current;
        m_skySunHistoryTarget = CaptureHistoryTarget(scene, index);
        m_skySunHistoryEpoch = m_historyEpoch;
    }

    if(!m_skySunEditing)
    {
        return;
    }

    if(cancelled)
    {
        ApplyObjectTransformEdit(scene, index, m_skySunUndoStart);
        m_skySunEditing = false;
        m_skySunHistoryTarget.reset();
        return;
    }

    Transform edited = current;
    edited.rotation = SunDirection::RotationFromAngles(SunAngles{std::clamp(m_skySunDrafts[0], -90.0f, 90.0f),
        std::fmod(std::fmod(m_skySunDrafts[1], 360.0f) + 360.0f, 360.0f)});

    if(std::isfinite(edited.rotation.x) && std::isfinite(edited.rotation.y))
    {
        ApplyObjectTransformEdit(scene, index, edited);
    }

    if(committed)
    {
        if(m_skySunHistoryTarget)
        {
            RecordTransformEdit(std::move(*m_skySunHistoryTarget), m_skySunHistoryEpoch, m_skySunUndoStart,
                static_cast<const Scene&>(scene.GetScene()).GetRenderObjects()[index].transform);
        }

        m_skySunEditing = false;
        m_skySunHistoryTarget.reset();
    }
}

void Editor::SyncSkyHdriChoices(const std::vector<EnvironmentAsset>& environments)
{
    m_skyHdriChoices.clear();

    for(const EnvironmentAsset& environment : environments)
    {
        m_skyHdriChoices.push_back({environment.displayName, environment.reference, !environment.relativePath.empty()});
    }
}

void Editor::DrawSkyHdriSection(IDemoScene& scene, bool editable)
{
    ImGui::PushID("HdriSky");
    Scene& runtime = scene.GetScene();
    SceneLightingSettings settings = runtime.GetLightingSettings();
    const std::string& reference = runtime.GetEnvironmentReference();

    // The HDRI: the Renderer's list (built-in HDRIs are imported into the project when chosen). Neutral is a Sky Type, not an
    // HDRI, so it is not offered here.
    std::string preview = reference.empty() ? std::string("(none chosen)") : reference;

    for(const SkyHdriChoice& choice : m_skyHdriChoices)
    {
        if(!choice.builtin && !choice.reference.empty() && choice.reference == reference)
        {
            preview = choice.displayName;
        }
    }

    if(ImGui::BeginCombo("HDRI Sky", preview.c_str()))
    {
        for(size_t index = 0; index < m_skyHdriChoices.size(); ++index)
        {
            const SkyHdriChoice& choice = m_skyHdriChoices[index];

            if(!choice.builtin && choice.reference.empty())
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(index));
            const bool selected = !choice.builtin && choice.reference == reference;

            if(ImGui::Selectable(choice.displayName.c_str(), selected) && !selected)
            {
                m_skyHdriRequest = index;
            }

            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    Tooltip("The scene's HDRI (formerly Debug > Rendering Settings > HDRI). A built-in HDRI is imported into the project's\n"
        "Textures/Environment folder when chosen. None is a black sky.");

    // Intensity Mode: Luminance (cd/m^2 per HDRI unit), the scene's environment luminance. Commit on release: each change resamples
    // the HDRI.
    ImGui::InputFloat("Intensity (cd/m^2 per HDRI unit)", &settings.environmentLuminanceScale, 0.0f, 0.0f, "%.6g");
    const bool intensityCommitted = ImGui::IsItemDeactivatedAfterEdit();
    Tooltip("HDRP's Intensity Mode Luminance: the luminance one HDRI texel unit stands for (formerly Scene Lighting >\n"
        "Environment Luminance).");
    ImGui::DragFloat("Rotation (deg)", &settings.environmentRotationDegrees, 0.5f, -SceneLightingSettings::MaximumEnvironmentRotationDegrees,
        SceneLightingSettings::MaximumEnvironmentRotationDegrees, "%.1f");
    const bool rotationCommitted = ImGui::IsItemDeactivatedAfterEdit();
    Tooltip("Yaw of the HDRI about the up axis. Applied when the HDRI is resampled into the sky, so it costs nothing per\n"
        "frame; the new rotation shows once the HDRI is prepared again (a fraction of a second).");

    const char* reason = nullptr;

    if(editable && (intensityCommitted || rotationCommitted))
    {
        if(ValidateSceneLightingSettings(settings, &reason))
        {
            runtime.SetLightingSettings(settings);
            m_skyStatus.clear();
        }
        else
        {
            m_skyStatus = reason ? reason : "Invalid HDRI sky settings.";
        }
    }

    ImGui::TextDisabled("Resolution: Project Settings > Rendering > Sky (HDRI sky quality).");
    ImGui::PopID();
}

void Editor::DrawSkyWindow(IDemoScene& scene)
{
    if(!m_showSkyWindow)
    {
        m_physicalSkyDraft.reset();
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const SceneDocument* document = owner ? owner->GetSceneDocument() : nullptr;
    const std::string title = std::string("Sky - ") + (document ? document->name : std::string("no level")) + "###LevelSky";
    ImGui::SetNextWindowSize(ImVec2(470.0f, 640.0f), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin(title.c_str(), &m_showSkyWindow))
    {
        ImGui::End();
        return;
    }

    // Stage 1: what can be edited. Procedural demo scenes have no document; packaged scenes are cooked.
    if(!owner || !document)
    {
        ImGui::TextWrapped("Open a project scene to edit its sky. Procedural demo scenes are lit by the HDRI chosen in Debug > "
            "Rendering Settings.");
        ImGui::End();
        return;
    }

    if(owner->IsArchiveActivation())
    {
        ImGui::TextWrapped("A packaged scene's sky is cooked with it and cannot be edited here.");
        ImGui::End();
        return;
    }

    const CompositeWorld* composite = owner->GetCompositeWorld();
    const EffectiveSceneEnvironment& effective = owner->GetEffectiveEnvironment();
    const bool localLevel = document->kind == SceneKind::Local && !composite;
    const bool inherited = localLevel && !document->environmentOverrides.sky;
    const bool editable = !inherited && !IsSceneEditingLocked() && m_gameState == GameState::Editing && !m_sceneOperationPending;
    const ImVec4 warning(1.0f, 0.75f, 0.3f, 1.0f);

    // Stage 2: the source of the values shown, the Override switch (Local Levels) and Reset to defaults.
    ImGui::SeparatorText("Source");
    std::string inheritedFrom = "the Engine defaults";

    if(composite)
    {
        ImGui::TextWrapped("World '%s': its own sky (this World preview edits the World).", composite->worldName.c_str());
    }
    else if(document->kind == SceneKind::World)
    {
        ImGui::TextWrapped("World '%s': its own sky.", document->name.c_str());
    }
    else if(effective.source == EffectiveSceneEnvironment::Source::AssignedWorld)
    {
        inheritedFrom = "World '" + effective.worldName + "'";
        ImGui::TextWrapped("Local Level in %s.", inheritedFrom.c_str());
    }
    else if(effective.source == EffectiveSceneEnvironment::Source::InvalidWorld)
    {
        ImGui::TextColored(warning, "Owning World unavailable: %s The Engine defaults are inherited.", effective.worldProblem.c_str());
    }
    else
    {
        ImGui::TextWrapped("Local Level without a World: an inherited sky is the Engine defaults.");
    }

    const SceneSky stored{document->skyType, document->physicalSky};
    SceneSky requested = stored;
    bool immediate = false;
    bool resetRequested = false;

    if(localLevel)
    {
        bool overrideSky = document->environmentOverrides.sky;
        ImGui::BeginDisabled(IsSceneEditingLocked() || m_sceneOperationPending);

        if(ImGui::Checkbox("Override", &overrideSky))
        {
            std::string error;
            m_skyStatus = CommitEnvironmentOverride(scene, static_cast<int>(Group::Sky), overrideSky, &error) ? std::string{} : error;
        }

        ImGui::EndDisabled();
        Tooltip("Off: this Local Level shows and uses its World's sky (or the Engine defaults). On: its own sky, edited here. The\n"
            "same switch as Sky in Environment (Local Level); either way no stored value is lost.");
        ImGui::SameLine();
    }

    ImGui::BeginDisabled(!editable);

    if(ImGui::Button("Reset to defaults"))
    {
        // The selected Model type's Earth values.
        PhysicalSkySettings defaults;
        defaults.model = document->physicalSky.value_or(PhysicalSkySettings{}).model;
        requested.physical = defaults;
        resetRequested = true;
        immediate = true;
    }

    ImGui::EndDisabled();

    if(inherited)
    {
        ImGui::TextDisabled("Inherited from %s: read-only until Override is ticked.", inheritedFrom.c_str());
    }

    // Stage 3: the values shown: the effective ones while inherited, else the document's (or the held drag's draft).
    const std::uint64_t runtimeIdentity = owner->GetRuntimeIdentity();
    const bool draftActive = editable && m_physicalSkyDraft && m_physicalSkyDraft->runtimeIdentity == runtimeIdentity;
    const SkyType shownType = inherited ? EffectiveSkyType(effective) : SceneDocumentSkyType(*document);
    const std::optional<PhysicalSkySettings>& shownPhysical = inherited ? effective.physicalSky : document->physicalSky;
    PhysicalSkySettings edited = draftActive ? m_physicalSkyDraft->settings : shownPhysical.value_or(PhysicalSkySettings{});
    SkyType editedType = shownType;
    bool typeChanged = false;
    SkyFieldResults fields;

    if(!draftActive)
    {
        m_physicalSkyDraft.reset();
    }

    ImGui::BeginDisabled(!editable);

    // Stage 4: Visual Environment.
    if(ImGui::CollapsingHeader("Visual Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID("VisualEnvironment");
        int typeIndex = SkyTypeIndex(editedType);

        if(ImGui::Combo("Sky Type", &typeIndex, "Physically Based Sky\0HDRI Sky\0Neutral\0"))
        {
            editedType = SkyTypeOrder[typeIndex];
            typeChanged = editedType != shownType;
            immediate = true;
        }

        Tooltip("Physically Based Sky: Engine's atmosphere, baked into the environment every lighting mode uses. HDRI Sky: the\n"
            "scene's HDRI. Neutral: the uniform grey environment of scenes without a sky.");
        Track(fields, InspectorDragFloats("Planet Radius (km)", &edited.planetRadiusKm, 1, PlanetRadiusDrag));
        Tooltip("Radius of the planet's ground sphere. Earth: 6,360 km.");
        Track(fields, InspectorDragFloats("Sea Level (m)", &edited.seaLevelMeters, 1, SeaLevelDrag));
        Tooltip("World height (Y) of the planet's top. The camera sees the sky from max(camera Y - sea level, 0.5 m).");

        if(editedType == SkyType::Hdri)
        {
            ImGui::TextWrapped("HDRI Sky: the HDRI, its intensity and rotation are in the HDRI Sky section below.");
        }
        else if(editedType == SkyType::Neutral)
        {
            ImGui::TextWrapped("Neutral: the uniform grey environment used by scenes without a sky.");
        }

        ImGui::PopID();
    }

    // Stage 4b (S4): HDRI Sky, HDRP's HDRI Sky override. Its edits apply at once (the HDRI is resampled into the sky).
    if(editedType == SkyType::Hdri && shownType == SkyType::Hdri && ImGui::CollapsingHeader("HDRI Sky", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSkyHdriSection(scene, editable);
    }

    // Stage 5: Physically Based Sky, in HDRP's sections. Earth (Simple) hides the altitudes, Earth (Advanced) shows them, Custom
    // Planet adds Air.
    if(editedType == SkyType::Physical && ImGui::CollapsingHeader("Physically Based Sky", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID("PhysicallyBasedSky");

        if(ImGui::TreeNodeEx("Model", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int model = static_cast<int>(edited.model);

            if(ImGui::Combo("Type", &model, "Earth (Simple)\0Earth (Advanced)\0Custom Planet\0"))
            {
                edited.model = static_cast<PhysicalSkyModel>(model);
                immediate = true;
            }

            Tooltip("Which settings are shown: Earth (Simple) the essentials, Earth (Advanced) also the altitudes, Custom Planet\n"
                "also the air. The sky always uses every stored value.");

            if(ImGui::Checkbox("Atmospheric Scattering", &edited.atmosphericScattering))
            {
                immediate = true;
            }

            Tooltip("Aerial perspective on surfaces: distant geometry fades into the sky's colour.");
            ImGui::TreePop();
        }

        const bool advanced = edited.model != PhysicalSkyModel::EarthSimple;
        const bool customPlanet = edited.model == PhysicalSkyModel::CustomPlanet;

        if(ImGui::TreeNodeEx("Planet", ImGuiTreeNodeFlags_DefaultOpen))
        {
            Track(fields, InspectorDragFloats("Ground Tint", edited.groundAlbedo.data(), 3, UnitDrag));
            Tooltip("Lambertian albedo of the planet ground (RGB). Earth: 0.3.");

            if(ImGui::Checkbox("Show Ground Below Horizon", &edited.showGroundBelowHorizon))
            {
                immediate = true;
            }

            Tooltip("Off: the lower half of the sky is black, for scenes with their own terrain.");
            ImGui::TreePop();
        }

        if(customPlanet && ImGui::TreeNodeEx("Air", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawDensityAndTint(fields, edited.rayleighScattering, edited.rayleighAbsorption, edited.rayleighScaleHeightKm,
                "Rayleigh optical depth at the zenith per channel (sigma_t x scale height). Earth: 0.0464, 0.1085, 0.2648.");
            Track(fields, InspectorDragFloats("Scale Height (km)", &edited.rayleighScaleHeightKm, 1, ScaleHeightDrag));
            Tooltip("Exponential scale height of the air density (HDRP's Maximum Altitude). Earth: 8 km.");
            ImGui::TreePop();
        }

        if(ImGui::TreeNodeEx("Aerosols", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawDensityAndTint(fields, edited.mieScattering, edited.mieAbsorption, edited.mieScaleHeightKm,
                "Mie optical depth at the zenith per channel (sigma_t x scale height). Earth: 0.0101.");
            Track(fields, InspectorDragFloats("Anisotropy", &edited.mieAnisotropy, 1, AnisotropyDrag));
            Tooltip("Mie phase g (Cornette-Shanks), from -1 to 1: positive scatters forward (the bright halo around the sun). Earth: 0.8.");

            if(advanced)
            {
                Track(fields, InspectorDragFloats("Scale Height (km)", &edited.mieScaleHeightKm, 1, ScaleHeightDrag));
                Tooltip("Exponential scale height of the aerosol density. Earth: 1.2 km.");
            }

            ImGui::TreePop();
        }

        if(ImGui::TreeNodeEx("Ozone", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // The dimmer is a view: the multiple of Earth's ozone in its strongest (green) channel; editing it sets Earth's
            // proportions.
            const float earthGreen = PhysicalSkySettings::EarthOzoneAbsorption[1];
            float dimmer = float(double(edited.ozoneAbsorption[1]) / double(earthGreen));
            const float shownDimmer = dimmer;
            Track(fields, InspectorDragFloats("Density Dimmer", &dimmer, 1, OzoneDimmerDrag));
            Tooltip("Multiple of Earth's ozone: 1 is Earth, 0 none. Setting it gives Earth's proportions per channel.");

            if(dimmer != shownDimmer)
            {
                for(std::size_t channel = 0; channel < 3; ++channel)
                {
                    edited.ozoneAbsorption[channel] = float(double(PhysicalSkySettings::EarthOzoneAbsorption[channel]) * double(dimmer));
                }
            }

            if(advanced)
            {
                Track(fields, InspectorDragFloats("Minimum Altitude (km)", &edited.ozoneMinimumAltitudeKm, 1, OzoneAltitudeDrag));
                Tooltip("Where the ozone layer starts. Earth: 10 km.");
                Track(fields, InspectorDragFloats("Layer Width (km)", &edited.ozoneLayerWidthKm, 1, OzoneAltitudeDrag));
                Tooltip("The layer's thickness; its density is a tent that peaks at half the width. Earth: 30 km (peak at 25 km).");
            }

            ImGui::TreePop();
        }

        if(ImGui::TreeNodeEx("Artistic Overrides"))
        {
            ImGui::TextWrapped("Non-physical. Applied to the visible sky and to the lighting it gives, so they always match.");
            Track(fields, InspectorDragFloats("Color Saturation", &edited.colorSaturation, 1, UnitDrag));
            Tooltip("1 keeps the sky's colour; 0 makes it grey.");
            Track(fields, InspectorDragFloats("Horizon Tint", edited.horizonTint.data(), 3, UnitDrag));
            Track(fields, InspectorDragFloats("Zenith Tint", edited.zenithTint.data(), 3, UnitDrag));
            Track(fields, InspectorDragFloats("Horizon Zenith Shift", &edited.horizonZenithShift, 1, ShiftDrag));
            Tooltip("Moves the blend between the horizon and zenith tints: positive toward the horizon.");
            ImGui::TreePop();
        }

        if(ImGui::TreeNodeEx("Miscellaneous"))
        {
            int intensityMode = static_cast<int>(edited.intensityMode);

            if(ImGui::Combo("Intensity Mode", &intensityMode, "Exposure\0Multiplier\0"))
            {
                edited.intensityMode = static_cast<PhysicalSkyIntensityMode>(intensityMode);
                immediate = true;
            }

            if(edited.intensityMode == PhysicalSkyIntensityMode::Exposure)
            {
                Track(fields, InspectorDragFloats("Exposure Compensation (EV)", &edited.exposureCompensation, 1, ExposureDrag));
                Tooltip("Scales the sky (not the sun) by 2^EV.");
            }
            else
            {
                Track(fields, InspectorDragFloats("Multiplier", &edited.multiplier, 1, MultiplierDrag));
                Tooltip("Scales the sky (not the sun).");
            }

            Track(fields, InspectorDragFloats("Multiple Scattering", &edited.multipleScattering, 1, MultipleScatteringDrag));
            Tooltip("Higher-order scattering: 1 is physical, 0 single scattering only, up to 2.");

            if(ImGui::Checkbox("Sample Below Horizon", &edited.sampleBelowHorizon))
            {
                immediate = true;
            }

            Tooltip("Importance-sample the ground below the horizon too (path tracers). Off: the lower half is never sampled.");
            int updateMode = static_cast<int>(edited.updateMode);

            if(ImGui::Combo("Update Mode", &updateMode, "On Changed\0On Demand\0Realtime\0"))
            {
                edited.updateMode = static_cast<PhysicalSkyUpdateMode>(updateMode);
                immediate = true;
            }

            Tooltip("On Changed: re-bakes when the sky, the sun or the camera altitude changes (over three frames).\n"
                "On Demand: only when Update Now is pressed. Realtime: also every Update Period (0: every frame).");

            if(edited.updateMode == PhysicalSkyUpdateMode::Realtime)
            {
                Track(fields, InspectorDragFloats("Update Period (s)", &edited.updatePeriodSeconds, 1, PeriodDrag));
            }

            if(ImGui::Button("Update Now"))
            {
                m_skyUpdateRequested = true;
            }

            ImGui::TreePop();
        }

        // S5 (§14.1): the night sky. HDRP's Space section, procedural.
        if(ImGui::TreeNodeEx("Night Sky"))
        {
            Track(fields, InspectorDragFloats("Night Sky Intensity", &edited.nightSkyIntensity, 1, NightIntensityDrag));
            Tooltip("Scales the stars and the airglow (1 is physical).");
            Track(fields, InspectorDragFloats("Airglow (cd/m^2)", &edited.airglowLuminance, 1, AirglowDrag));
            Tooltip("The faint glow of the upper atmosphere at the zenith, brighter toward the horizon. Earth: about 0.001 cd/m^2.");
            Track(fields, InspectorDragFloats("Planet Rotation (deg)", &edited.planetRotationDegrees, 1, RotationDrag));
            Tooltip("Turns the stars about the vertical axis.");
            ImGui::TextWrapped("The moon is the second Directional Light with Affect Physically Based Sky on. Night scenes need a low "
                "reference exposure (about EV 0 under a moon): fp16 cannot hold a night sky at EV 15.");
            ImGui::TreePop();
        }

        if(ImGui::TreeNodeEx("Physical Coefficients"))
        {
            ImGui::TextWrapped("The same stored values in Unreal's form: coefficients in 10^-6 per metre, heights in km.");
            DrawMicroCoefficients(fields, "Rayleigh Scattering", edited.rayleighScattering);
            DrawMicroCoefficients(fields, "Rayleigh Absorption", edited.rayleighAbsorption);
            Track(fields, InspectorDragFloats("Rayleigh Scale Height (km)", &edited.rayleighScaleHeightKm, 1, ScaleHeightDrag));
            DrawMicroCoefficients(fields, "Mie Scattering", edited.mieScattering);
            DrawMicroCoefficients(fields, "Mie Absorption", edited.mieAbsorption);
            Track(fields, InspectorDragFloats("Mie Scale Height (km)", &edited.mieScaleHeightKm, 1, ScaleHeightDrag));
            DrawMicroCoefficients(fields, "Ozone Absorption", edited.ozoneAbsorption);
            Track(fields, InspectorDragFloats("Ozone Minimum Altitude (km)", &edited.ozoneMinimumAltitudeKm, 1, OzoneAltitudeDrag));
            Track(fields, InspectorDragFloats("Ozone Layer Width (km)", &edited.ozoneLayerWidthKm, 1, OzoneAltitudeDrag));
            Track(fields, InspectorDragFloats("Atmosphere Height (km)", &edited.atmosphereHeightKm, 1, AtmosphereHeightDrag));
            Tooltip("Height of the atmosphere's top above the ground. Earth: 100 km.");
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    // Stage 5b (S5, §14.2): Clouds, a list of at most four layers, each a Cloud Layer (2D) or Volumetric clouds with HDRP's Simple,
    // Advanced and Manual control modes.
    if(editedType == SkyType::Physical && ImGui::CollapsingHeader("Clouds", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID("Clouds");

        if(!m_cloudPresetsLoaded)
        {
            m_cloudPresetsLoaded = true;
            m_cloudPresets = CloudPresets::Load(WindowsWindow::GetExecutableDirectory() / "assets" / "Sky" / "CloudPresets.json",
                &m_cloudPresetError);
        }

        if(!m_cloudPresetError.empty())
        {
            ImGui::TextColored(warning, "%s", m_cloudPresetError.c_str());
        }

        std::optional<std::size_t> removed;

        for(std::size_t index = 0; index < edited.cloudLayers.size(); ++index)
        {
            CloudLayerSettings& layer = edited.cloudLayers[index];
            const bool sheet = layer.kind == CloudLayerKind::Layer;
            ImGui::PushID(static_cast<int>(index));
            const std::string header = "Layer " + std::to_string(index + 1) + ": " + layer.preset + (sheet ? " (Cloud Layer)" :
                " (Volumetric)");

            if(ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                int kind = sheet ? 0 : 1;

                if(ImGui::Combo("Type", &kind, "Cloud Layer (2D)\0Volumetric Clouds\0"))
                {
                    layer.kind = kind == 0 ? CloudLayerKind::Layer : CloudLayerKind::Volumetric;
                    layer.thicknessKm = kind == 0 ? 0.0f : (std::max)(layer.thicknessKm, 2.0f);
                    immediate = true;
                }

                Tooltip("Cloud Layer: a sheet at one altitude, cheap (HDRP's Cloud Layer). Volumetric Clouds: a shell of 3D cloud "
                    "between two altitudes, ray-marched (HDRP's Volumetric Clouds).");

                if(layer.kind == CloudLayerKind::Volumetric)
                {
                    int mode = static_cast<int>(layer.controlMode);

                    if(ImGui::Combo("Control Mode", &mode, "Simple\0Advanced\0Manual\0"))
                    {
                        layer.controlMode = static_cast<CloudControlMode>(mode);
                        immediate = true;
                    }

                    Tooltip("What this window shows: Simple a preset and a few amounts; Advanced the altitudes, shape and lighting; "
                        "Manual every noise parameter. The stored values are the same.");
                }

                // The presets of this layer's kind (data: assets/Sky/CloudPresets.json).
                if(ImGui::BeginCombo("Preset", layer.preset.c_str()))
                {
                    for(const CloudPreset& preset : m_cloudPresets)
                    {
                        if(preset.layer.kind != layer.kind)
                        {
                            continue;
                        }

                        if(ImGui::Selectable(preset.name.c_str(), preset.name == layer.preset))
                        {
                            CloudPresets::Apply(preset, layer);
                            immediate = true;
                        }
                    }

                    ImGui::EndCombo();
                }

                const bool advanced = layer.kind == CloudLayerKind::Layer || layer.controlMode != CloudControlMode::Simple;
                const bool manual = layer.kind == CloudLayerKind::Layer || layer.controlMode == CloudControlMode::Manual;
                Track(fields, InspectorDragFloats("Coverage", &layer.coverage, 1, UnitDrag));
                Tooltip("The share of the sky this layer covers.");
                Track(fields, InspectorDragFloats(sheet ? "Optical Depth" : "Density (per km)", &layer.density, 1, CloudDensityDrag));
                Tooltip(sheet ? "The sheet's optical depth where it is fully dense." :
                    "Extinction per kilometre where the cloud is fully dense; real cumulus is about 20 to 100.");
                Track(fields, InspectorDragFloats("Wind Speed (m/s)", &layer.windSpeedMetersPerSecond, 1, WindSpeedDrag));
                Track(fields, InspectorDragFloats("Wind Direction (deg)", &layer.windDirectionDegrees, 1, WindDirectionDrag));
                Tooltip("The direction the clouds move toward, from +X toward +Z.");

                if(advanced)
                {
                    Track(fields, InspectorDragFloats(sheet ? "Altitude (km)" : "Bottom Altitude (km)", &layer.altitudeKm, 1,
                        CloudAltitudeDrag));

                    if(!sheet)
                    {
                        Track(fields, InspectorDragFloats("Thickness (km)", &layer.thicknessKm, 1, CloudThicknessDrag));
                        Track(fields, InspectorDragFloats("Shape Factor", &layer.shapeFactor, 1, UnitDrag));
                        Tooltip("0: flat bottoms and tops (stratus); 1: tall, rounded tops (cumulus).");
                    }

                    Track(fields, InspectorDragFloats("Erosion", &layer.erosion, 1, UnitDrag));
                    Tooltip("How much the fine detail noise eats into the edges.");
                    Track(fields, InspectorDragFloats("Albedo", layer.albedo.data(), 3, UnitDrag));
                    Track(fields, InspectorDragFloats("Multiple Scattering", &layer.multipleScattering, 1, UnitDrag));
                    Tooltip("The strength of the light scattered more than once (0 to 1): brighter, softer clouds.");
                    Track(fields, InspectorDragFloats("Ambient Strength", &layer.ambientStrength, 1, CloudAmbientDrag));
                    bool shadows = layer.castShadows;

                    if(ImGui::Checkbox("Cast Shadows", &shadows))
                    {
                        layer.castShadows = shadows;
                        immediate = true;
                    }
                }

                if(manual)
                {
                    Track(fields, InspectorDragFloats("Shape Scale (km)", &layer.shapeScaleKm, 1, CloudShapeScaleDrag));
                    Track(fields, InspectorDragFloats("Detail Scale (km)", &layer.detailScaleKm, 1, CloudDetailScaleDrag));
                    Track(fields, InspectorDragFloats("Phase Forward", &layer.phaseForward, 1, CloudForwardDrag));
                    Track(fields, InspectorDragFloats("Phase Backward", &layer.phaseBackward, 1, CloudBackwardDrag));
                    Track(fields, InspectorDragFloats("Phase Blend", &layer.phaseBlend, 1, UnitDrag));
                    Tooltip("The forward lobe's weight in the dual Henyey-Greenstein phase.");
                }

                ImGui::TextDisabled("Coverage map: procedural (the weather-map slot is stored for later).");

                if(ImGui::SmallButton("Remove Layer"))
                {
                    removed = index;
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if(removed)
        {
            edited.cloudLayers.erase(edited.cloudLayers.begin() + static_cast<std::ptrdiff_t>(*removed));
            immediate = true;
        }

        ImGui::BeginDisabled(edited.cloudLayers.size() >= PhysicalSkySettings::MaximumCloudLayers);

        if(ImGui::Button("Add Volumetric Layer"))
        {
            CloudLayerSettings layer;
            float top = 0.0f;

            for(const CloudLayerSettings& existing : edited.cloudLayers)
            {
                top = (std::max)(top, existing.altitudeKm + existing.thicknessKm);
            }

            for(const CloudPreset& preset : m_cloudPresets)
            {
                if(preset.name == "Cloudy")
                {
                    CloudPresets::Apply(preset, layer);
                }
            }

            layer.altitudeKm = (std::max)(layer.altitudeKm, edited.cloudLayers.empty() ? 0.0f : top + 0.5f);
            edited.cloudLayers.push_back(layer);
            immediate = true;
        }

        ImGui::SameLine();

        if(ImGui::Button("Add Cloud Layer"))
        {
            CloudLayerSettings layer;
            layer.kind = CloudLayerKind::Layer;
            layer.thicknessKm = 0.0f;
            float top = 0.0f;

            for(const CloudLayerSettings& existing : edited.cloudLayers)
            {
                top = (std::max)(top, existing.altitudeKm + existing.thicknessKm);
            }

            for(const CloudPreset& preset : m_cloudPresets)
            {
                if(preset.name == "Cirrus")
                {
                    CloudPresets::Apply(preset, layer);
                }
            }

            layer.altitudeKm = (std::max)(layer.altitudeKm, edited.cloudLayers.empty() ? 0.0f : top + 0.5f);
            edited.cloudLayers.push_back(layer);
            immediate = true;
        }

        ImGui::EndDisabled();
        ImGui::TextDisabled("At most 4 layers, bottom to top, not overlapping. Clouds render with the physically based sky.");
        ImGui::PopID();
    }

    ImGui::EndDisabled();

    if(!m_skyStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_skyStatus.c_str());
    }

    if(SceneDocumentSkyType(*document) == SkyType::Physical || EffectiveSkyType(effective) == SkyType::Physical)
    {
        DrawSkySunSection(scene, *owner);
    }

    ImGui::End();

    if(!editable)
    {
        return;
    }

    // Stage 6: route the result. A held drag only updates the draft the renderer bakes (an invalid value keeps the last valid
    // draft and says why); a completed edit, a combo, a checkbox or the reset is one history entry.
    if(fields.cancelled)
    {
        m_physicalSkyDraft.reset();
    }

    if(fields.editing)
    {
        std::string error;

        if(ValidatePhysicalSkySettings(edited, &error))
        {
            m_physicalSkyDraft = PhysicalSkyDraft{runtimeIdentity, edited};
            m_skyStatus.clear();
        }
        else
        {
            m_skyStatus = error;
        }

        return;
    }

    if(!fields.committed && !immediate)
    {
        return;
    }

    m_physicalSkyDraft.reset();
    // A chosen type is stored; otherwise the document keeps its own (possibly derived) type.
    if(typeChanged)
    {
        requested.type = editedType;
    }

    // The physical sky is stored once it differs from what was shown (its defaults when absent), so a type change alone never
    // creates the block. Reset to defaults already chose its value.
    if(!resetRequested && !(edited == shownPhysical.value_or(PhysicalSkySettings{})))
    {
        requested.physical = edited;
    }

    std::string error;
    m_skyStatus = CommitSky(scene, stored, requested, &error) ? std::string{} : error;
}
