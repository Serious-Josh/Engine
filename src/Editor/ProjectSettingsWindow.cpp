#include "Editor/Editor.h"
#include "Editor/SteamAudioSettingsStatus.h"
#include "Debug/DebugUI.h"
#include "Debug/LightingModeTransitionPresentation.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Engine/RenderingOverrides.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <system_error>

namespace
{
    // Bounds the listing of a very large Scenes tree; selection still validates any listed scene.
    constexpr std::size_t kMaximumListedScenes = 2048;

    std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    bool IsSceneFile(const std::filesystem::path& path)
    {
        const auto extension = path.extension().native();
        return CompareStringOrdinal(extension.c_str(), -1, L".scene", -1, TRUE) == CSTR_EQUAL;
    }

    // The active editable scene when it has been saved to a file; null otherwise.
    const FileBackedDemoScene* SavedActiveScene(const IDemoScene& demoScene)
    {
        const auto* scene = dynamic_cast<const FileBackedDemoScene*>(&demoScene);

        if(!scene || scene->IsArchiveActivation() || scene->IsUnsaved())
        {
            return nullptr;
        }

        return scene;
    }

    // PS.3 draft controls: each edits the draft value in place and returns whether it changed. The combo index is
    // the enum value; every project enum lists its choices in display order.
    template<typename Enum, std::size_t Count>
    bool DraftCombo(const char* label, Enum& value, const char* const (&items)[Count])
    {
        int selection = static_cast<int>(value);

        if(!ImGui::Combo(label, &selection, items, static_cast<int>(Count)))
        {
            return false;
        }

        value = static_cast<Enum>(selection);
        return true;
    }

    bool DraftSliderUInt(const char* label, std::uint32_t& value, std::uint32_t minimum, std::uint32_t maximum,
        ImGuiSliderFlags flags = 0)
    {
        return ImGui::SliderScalar(label, ImGuiDataType_U32, &value, &minimum, &maximum, "%u",
            flags | ImGuiSliderFlags_AlwaysClamp);
    }

    bool DraftSliderFloat(const char* label, float& value, float minimum, float maximum, const char* format,
        ImGuiSliderFlags flags = 0)
    {
        return ImGui::SliderFloat(label, &value, minimum, maximum, format, flags | ImGuiSliderFlags_AlwaysClamp);
    }

    void Tooltip(const char* text)
    {
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    // A Debug panel session override of this setting; the saved value shown here is not what renders.
    void SessionOverrideNote(const DebugUI& debugUI, RenderingOverrideField field)
    {
        if(!IsOverridden(debugUI.GetRenderingOverrides(), field))
        {
            return;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(session override)");
        Tooltip("The Debug panel overrides this setting for this session, so the renderer uses its value instead. "
            "Saving a different value here clears that override.");
    }

    const char* PsrChainName(std::uint32_t chain)
    {
        if(chain >= 2u)
        {
            return "two-bounce";
        }

        return chain == 1u ? "single-bounce" : "off";
    }
}

void Editor::OnStartupSceneChanged(bool succeeded, std::string message)
{
    m_projectSettingsMessage = std::move(message);
    m_projectSettingsMessageIsError = !succeeded;
}

void Editor::ListStartupSceneCandidates()
{
    m_startupSceneCandidates.clear();
    m_startupSceneCandidatesListed = true;

    if(!m_activeProject)
    {
        return;
    }

    // File names only: scene documents are validated when one is chosen, not while listing.
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator entry(m_activeProject->GetPaths().GetScenesDirectory(), options, error);
    const std::filesystem::recursive_directory_iterator end;

    while(!error && entry != end && m_startupSceneCandidates.size() < kMaximumListedScenes)
    {
        std::error_code entryError;
        const auto type = entry->symlink_status(entryError).type();

        if(type == std::filesystem::file_type::symlink || type == std::filesystem::file_type::junction)
        {
            // Links and junctions are not followed; containment is checked again on selection.
            entry.disable_recursion_pending();
        }
        else if(type == std::filesystem::file_type::regular && IsSceneFile(entry->path()))
        {
            m_startupSceneCandidates.push_back(entry->path());
        }

        entry.increment(error);
    }

    std::sort(m_startupSceneCandidates.begin(), m_startupSceneCandidates.end());
}

void Editor::OpenProjectSettings()
{
    m_showProjectSettings = true;
    m_startupSceneCandidatesListed = false;

    // Reopening keeps an unsaved draft; otherwise the draft starts from the saved settings.
    if(!m_projectSettingsDraft && m_activeProject)
    {
        m_projectSettingsDraft = m_activeProject->GetProjectSettings().settings;
    }
}

bool Editor::HasUnsavedProjectSettings() const
{
    return m_projectSettingsDraft && m_activeProject &&
        !(*m_projectSettingsDraft == m_activeProject->GetProjectSettings().settings);
}

void Editor::CloseProjectSettings()
{
    m_showProjectSettings = false;
    m_projectSettingsDraft.reset();
    m_projectSettingsCloseAction = ProjectSettingsCloseAction::None;
    m_projectSettingsCorrections.clear();
    m_renderingSettingsMessage.clear();
}

std::string Editor::ProjectSettingsSaveBlocker() const
{
    if(!m_activeProject)
    {
        return "No project is open.";
    }

    if(!m_activeProject->GetManifest())
    {
        return "This project has no .engineproj manifest, so its settings cannot be saved.";
    }

    if(!m_projectOwnershipMessage.empty())
    {
        return "Project ownership could not be verified; project changes are disabled.";
    }

    const LoadedProjectSettings& loaded = m_activeProject->GetProjectSettings();

    if(loaded.status == PersonalSettingsFile::Status::UnsupportedVersion)
    {
        return "ProjectSettings.json was written by a newer Engine version and is read-only here.";
    }

    if(loaded.IsReadOnly())
    {
        return "ProjectSettings.json could not be read, so it is read-only for this session.";
    }

    if(m_physicsState != PhysicsState::Stopped)
    {
        return "Stop Physics Simulation to change project settings.";
    }

    if(m_gameState != GameState::Editing)
    {
        return "Stop Game Play to change project settings.";
    }

    if(m_projectSettingsSavePending)
    {
        return "Saving...";
    }

    return {};
}

void Editor::RequestProjectSettingsSave(ProjectSettingsCloseAction closeAfterSave)
{
    if(!m_projectSettingsDraft || !ProjectSettingsSaveBlocker().empty())
    {
        return;
    }

    // The same validation the save applies, done first so its corrections can be reported.
    ProjectSettings validated = *m_projectSettingsDraft;
    m_projectSettingsCorrections.clear();
    ValidateProjectSettings(validated, &m_projectSettingsCorrections);

    m_projectSettingsSaveRequest = std::move(validated);
    m_projectSettingsSavePending = true;
    m_projectSettingsCloseAfterSave = closeAfterSave;
    m_renderingSettingsMessage.clear();
}

void Editor::OnProjectSettingsSaveCompleted(bool succeeded, std::string message)
{
    m_projectSettingsSavePending = false;
    const ProjectSettingsCloseAction closeAfterSave =
        std::exchange(m_projectSettingsCloseAfterSave, ProjectSettingsCloseAction::None);

    if(!succeeded)
    {
        // The draft, the saved settings and the renderer are unchanged; a held project transition is cancelled.
        m_renderingSettingsMessage = "Project settings were not saved: " + message;
        m_renderingSettingsMessageIsError = true;

        if(closeAfterSave == ProjectSettingsCloseAction::LeaveProject && m_fileRequest)
        {
            m_fileRequest.reset();
            ReportLifecycleMessage("Project Settings could not be saved, so the project was not closed.");
        }

        return;
    }

    // The saved (validated) settings become the draft: no unsaved changes remain.
    if(m_activeProject)
    {
        m_projectSettingsDraft = m_activeProject->GetProjectSettings().settings;
    }

    m_renderingSettingsMessage = m_projectSettingsCorrections.empty() ? "Project settings saved." :
        "Project settings saved with corrections:";
    m_renderingSettingsMessageIsError = false;

    if(closeAfterSave == ProjectSettingsCloseAction::CloseWindow)
    {
        CloseProjectSettings();
    }
}

void Editor::DrawProjectSettings(const IDemoScene& demoScene, const DebugUI& debugUI)
{
    if(!m_showProjectSettings)
    {
        return;
    }

    // Categories added later (Physics, Audio, Input, ...) are one more row here and one draw function.
    struct Category
    {
        const char* name;
        void (Editor::*draw)(const DebugUI&, bool);
    };
    static constexpr std::array<Category, 2> categories{{{"Rendering", &Editor::DrawRenderingSettingsCategory},
        {"Audio", &Editor::DrawAudioSettingsCategory}}};
    const std::size_t categoryIndex = (std::min)(m_projectSettingsCategory, categories.size() - 1);
    const bool audioCategory = categories[categoryIndex].draw == &Editor::DrawAudioSettingsCategory;

    ImGui::SetNextWindowSize(ImVec2(860, 660), ImGuiCond_FirstUseEver);
    bool open = true;
    const bool visible = ImGui::Begin("Project Settings", &open);

    // The close button with unsaved changes asks first; without changes it closes at once.
    if(!open)
    {
        if(HasUnsavedProjectSettings())
        {
            if(m_projectSettingsCloseAction == ProjectSettingsCloseAction::None && !m_projectSettingsSavePending)
            {
                m_projectSettingsCloseAction = ProjectSettingsCloseAction::CloseWindow;
            }
        }
        else
        {
            CloseProjectSettings();
        }
    }

    if(!visible || !m_activeProject || !m_showProjectSettings)
    {
        ImGui::End();
        DrawProjectSettingsCloseConfirmation();
        return;
    }

    if(!m_projectSettingsDraft)
    {
        m_projectSettingsDraft = m_activeProject->GetProjectSettings().settings;
    }

    // Legacy section: project identity and the game startup scene, saved immediately to .engineproj. It is not
    // part of the draft below.
    if(ImGui::CollapsingHeader("Project and game startup scene"))
    {
        DrawStartupSceneSection(demoScene);
    }

    const std::string blocker = ProjectSettingsSaveBlocker();
    // Play and Physics Simulation own the scene and its rendering; the settings are read-only meanwhile.
    const bool editable = !IsSceneEditingLocked() && !m_projectSettingsSavePending;
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 3.0f;

    ImGui::BeginChild("ProjectSettingsCategories", ImVec2(170.0f, -footerHeight), ImGuiChildFlags_Borders);

    for(std::size_t index = 0; index < categories.size(); ++index)
    {
        if(ImGui::Selectable(categories[index].name, m_projectSettingsCategory == index))
        {
            m_projectSettingsCategory = index;
        }
    }

    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ProjectSettingsCategory", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
    const Category& category = categories[categoryIndex];
    (this->*category.draw)(debugUI, editable);
    ImGui::EndChild();

    // Footer: unsaved state and the draft transaction.
    const bool dirty = HasUnsavedProjectSettings();

    if(dirty)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Unsaved changes");
    }
    else
    {
        ImGui::TextDisabled("No unsaved changes");
    }

    ImGui::BeginDisabled(!blocker.empty());

    if(ImGui::Button("Save"))
    {
        RequestProjectSettingsSave(ProjectSettingsCloseAction::None);
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_projectSettingsSavePending);

    if(ImGui::Button("Cancel"))
    {
        CloseProjectSettings();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable);

    // Resets only the category on screen, so reverting Audio never touches the Rendering draft or the reverse.
    if(ImGui::Button("Revert to Defaults"))
    {
        if(audioCategory)
        {
            m_projectSettingsDraft->audio = AudioSettings{};
        }
        else
        {
            m_projectSettingsDraft->rendering = RenderingSettings{};
        }
    }

    Tooltip(audioCategory ? "Resets the Audio draft to the Engine defaults. Nothing changes until you save." :
        "Resets the Rendering draft to the Engine defaults. Nothing changes until you save.");
    ImGui::EndDisabled();

    if(!blocker.empty())
    {
        ImGui::TextWrapped("%s", blocker.c_str());
    }

    if(!m_renderingSettingsMessage.empty())
    {
        const ImVec4 errorColor(1.0f, 0.45f, 0.4f, 1.0f);
        const ImVec4 infoColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, m_renderingSettingsMessageIsError ? errorColor : infoColor);
        ImGui::TextWrapped("%s", m_renderingSettingsMessage.c_str());
        ImGui::PopStyleColor();

        for(const std::string& correction : m_projectSettingsCorrections)
        {
            ImGui::BulletText("%s", correction.c_str());
        }
    }

    ImGui::End();
    DrawProjectSettingsCloseConfirmation();
}

void Editor::DrawProjectSettingsCloseConfirmation()
{
    constexpr const char* popupName = "Unsaved Project Settings";

    if(m_projectSettingsCloseAction == ProjectSettingsCloseAction::None)
    {
        return;
    }

    if(!ImGui::IsPopupOpen(popupName))
    {
        ImGui::OpenPopup(popupName);
    }

    if(!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    const ProjectSettingsCloseAction action = m_projectSettingsCloseAction;
    ImGui::TextUnformatted("Project Settings has unsaved changes.");
    const std::string blocker = ProjectSettingsSaveBlocker();
    ImGui::BeginDisabled(!blocker.empty());

    if(ImGui::Button("Save"))
    {
        m_projectSettingsCloseAction = ProjectSettingsCloseAction::None;
        RequestProjectSettingsSave(action);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Discard"))
    {
        // A held project transition proceeds once the draft is gone.
        CloseProjectSettings();
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if(ImGui::Button("Keep Editing"))
    {
        if(action == ProjectSettingsCloseAction::LeaveProject)
        {
            m_fileRequest.reset();
        }

        m_projectSettingsCloseAction = ProjectSettingsCloseAction::None;
        ImGui::CloseCurrentPopup();
    }

    if(!blocker.empty())
    {
        ImGui::TextWrapped("Save is unavailable: %s", blocker.c_str());
    }

    ImGui::EndPopup();
}

void Editor::DrawRenderingSettingsCategory(const DebugUI& debugUI, bool editable)
{
    RenderingSettings& draft = m_projectSettingsDraft->rendering;
    using Field = RenderingOverrideField;
    using Legacy = LegacyRenderingSettings;
    using PathTracing = PathTracingRenderingSettings;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Saved in ProjectSettings.json and packaged by Build Game. Built games render every lighting "
        "mode, PSR, SHARC and DLSS. DLSS needs nvngx_dlss.dll beside the game's executable, which Build Game does not "
        "ship yet; without it the game renders at Native. Caustics are Editor-only.");
    ImGui::PopTextWrapPos();
    const std::size_t overrides = CountOverrides(debugUI.GetRenderingOverrides());

    if(overrides > 0)
    {
        ImGui::TextWrapped("%zu Debug panel session override%s active. Settings marked \"(session override)\" "
            "render with the Debug panel's value until it is cleared.", overrides, overrides == 1 ? " is" : "s are");
    }

    ImGui::BeginDisabled(!editable);

    ImGui::SeparatorText("General");
    const char* lightingModes[] = {"Legacy", "Reference Path Tracer", "Path Tracing (Raster Primary + ReSTIR DI)"};
    DraftCombo("Lighting Mode", draft.lightingMode, lightingModes);
    SessionOverrideNote(debugUI, Field::lightingMode);
    const char* toneMappers[] = {"AgX", "GT7"};
    DraftCombo("Tonemapper", draft.toneMapper, toneMappers);
    Tooltip("Final display mapping only; the lighting and accumulation are unchanged.");
    SessionOverrideNote(debugUI, Field::toneMapper);

    if(draft.lightingMode == ProjectLightingMode::Legacy)
    {
        ImGui::SeparatorText("Legacy");
        const char* reflectionDenoisers[] = {"NRD", "Native"};
        DraftCombo("Reflection denoiser", draft.legacy.reflectionDenoiser, reflectionDenoisers);
        SessionOverrideNote(debugUI, Field::reflectionDenoiser);
        ImGui::Checkbox("NRD contribution limiter", &draft.legacy.contributionLimiter);
        SessionOverrideNote(debugUI, Field::contributionLimiter);
        ImGui::Checkbox("Enable RTGI", &draft.legacy.rtgi);
        SessionOverrideNote(debugUI, Field::rtgi);
        DraftSliderFloat("RTGI Contact AO Strength", draft.legacy.rtgiContactAOStrength, Legacy::MinimumContactAOStrength,
            Legacy::MaximumContactAOStrength, "%.2f");
        SessionOverrideNote(debugUI, Field::rtgiContactAOStrength);
        DraftSliderFloat("RTGI Contact AO Radius", draft.legacy.rtgiContactAORadius, Legacy::MinimumContactAORadius,
            Legacy::MaximumContactAORadius, "%.2f");
        SessionOverrideNote(debugUI, Field::rtgiContactAORadius);
    }
    else if(draft.lightingMode == ProjectLightingMode::ReferencePathTracer)
    {
        ImGui::Spacing();
        ImGui::TextWrapped("The Reference Path Tracer uses fixed internal quality (1 sample per frame, depth 3, roulette "
            "from depth 3, 65536 samples) and is not affected by the Path Tracing settings.");
    }
    else
    {
        PathTracingRenderingSettings& pathTracing = draft.pathTracing;

        ImGui::SeparatorText("Quality");
        DraftSliderUInt("Samples per frame", pathTracing.samplesPerFrame, 1, PathTracing::MaximumSamplesPerFrame);
        SessionOverrideNote(debugUI, Field::samplesPerFrame);
        DraftSliderUInt("Max path depth", pathTracing.maxPathDepth, 1, PathTracing::MaximumPathDepth);
        SessionOverrideNote(debugUI, Field::maxPathDepth);
        DraftSliderUInt("Roulette from depth", pathTracing.rouletteFromDepth, 1, PathTracing::MaximumRouletteDepth);
        Tooltip("First path vertex subject to Russian roulette; above the max path depth, roulette is off.");
        SessionOverrideNote(debugUI, Field::rouletteFromDepth);
        DraftSliderUInt("Sample target", pathTracing.sampleTarget, 1, PathTracing::MaximumSampleTarget,
            ImGuiSliderFlags_Logarithmic);
        Tooltip("Accumulation stops at this many samples per pixel; every change restarts accumulation.");
        SessionOverrideNote(debugUI, Field::sampleTarget);

        if(pathTracing.sampleTarget < pathTracing.samplesPerFrame)
        {
            ImGui::TextDisabled("Saved as %u: the sample target is at least the samples per frame.",
                pathTracing.samplesPerFrame);
        }

        ImGui::SeparatorText("Primary Surface Replacement");
        const char* psrModes[] = {"Off", "Single-bounce", "Two-bounce"};
        DraftCombo("PSR mode", pathTracing.psrMode, psrModes);
        Tooltip("Lights surfaces seen through pure mirrors in place of the mirror (one or two mirror bounces).");
        SessionOverrideNote(debugUI, Field::psrMode);

        if(pathTracing.psrMode == ProjectPsrMode::TwoBounce && pathTracing.maxPathDepth < 3)
        {
            ImGui::TextDisabled("Two-bounce needs max path depth 3 or more; single-bounce is used meanwhile.");
        }

        ImGui::BeginDisabled(pathTracing.psrMode == ProjectPsrMode::Off);
        const char* psrDebugViews[] = {"Off", "Replacement Mask", "Reflected Normals", "Reflected Albedo"};
        DraftCombo("PSR Debug View", pathTracing.psrDebugView, psrDebugViews);
        Tooltip("Development view: replaces the image with the replaced G-buffer.");
        SessionOverrideNote(debugUI, Field::psrDebugView);
        ImGui::EndDisabled();

        ImGui::SeparatorText("DLSS Super Resolution");
        ImGui::Checkbox("Enable DLSS", &pathTracing.dlss);
        Tooltip("Needs a PT denoiser; DLSS activates once the lighting mode and the denoiser are prepared.");
        SessionOverrideNote(debugUI, Field::dlss);
        ImGui::BeginDisabled(!pathTracing.dlss);
        const char* dlssQualities[] = {"Quality", "Balanced", "Performance"};
        DraftCombo("DLSS quality", pathTracing.dlssQuality, dlssQualities);
        SessionOverrideNote(debugUI, Field::dlssQuality);
        ImGui::EndDisabled();

        ImGui::SeparatorText("Path tracing and denoising");
        ImGui::Checkbox("PT camera jitter (experimental)", &pathTracing.cameraJitter);
        SessionOverrideNote(debugUI, Field::cameraJitter);
        const char* denoisers[] = {"Off", "Engine", "NRD REBLUR (experimental)", "NRD RELAX (experimental)",
            "NRD Hybrid (experimental)"};
        DraftCombo("PT denoiser", pathTracing.denoiser, denoisers);
        Tooltip("NRD Hybrid (RELAX diffuse, REBLUR specular) is the default.");
        SessionOverrideNote(debugUI, Field::denoiser);
        const char* denoiserViews[] = {"Normal", "Temporal (demodulated)", "Spatial (demodulated)",
            "Spatial - temporal difference", "Estimated uncertainty", "History length", "Raw extraction"};
        DraftCombo("PT denoiser view", pathTracing.denoiserView, denoiserViews);
        Tooltip("Diagnostic views of the Engine denoiser's stages; Normal is the production image.");
        SessionOverrideNote(debugUI, Field::denoiserView);
        const char* signals[] = {"Diffuse lighting", "Specular"};
        DraftCombo("PT denoiser signal", pathTracing.denoiserSignal, signals);
        SessionOverrideNote(debugUI, Field::denoiserSignal);
        DraftSliderFloat("PT view exposure (stops)", pathTracing.viewExposureStops, PathTracing::MinimumViewExposureStops,
            PathTracing::MaximumViewExposureStops, "%.1f");
        SessionOverrideNote(debugUI, Field::viewExposureStops);
        DraftSliderFloat("PT difference amplification", pathTracing.differenceAmplification,
            PathTracing::MinimumDifferenceAmplification, PathTracing::MaximumDifferenceAmplification, "%.0f",
            ImGuiSliderFlags_Logarithmic);
        SessionOverrideNote(debugUI, Field::differenceAmplification);
        DraftSliderFloat("PT uncertainty floor (log10)", pathTracing.uncertaintyFloorLog10,
            PathTracing::MinimumUncertaintyFloorLog10, PathTracing::MaximumUncertaintyFloorLog10, "%.1f");
        SessionOverrideNote(debugUI, Field::uncertaintyFloorLog10);

        ImGui::SeparatorText("SHARC");
        ImGui::Checkbox("Enable SHARC (experimental)", &pathTracing.sharc);
        Tooltip("Cached radiance for path continuation. Its tuning stays in the Debug panel.");
        SessionOverrideNote(debugUI, Field::sharc);

        ImGui::SeparatorText("Dedicated Caustics");

        // The two caustics passes are exclusive; enabling one switches the other off.
        if(ImGui::Checkbox("Dedicated caustics (experimental)", &pathTracing.dedicatedCaustics) &&
            pathTracing.dedicatedCaustics)
        {
            pathTracing.pixelCentredCaustics = false;
        }

        Tooltip("Restricted domain: a registered static reflector and receiver, one point light, PT denoiser Off.");
        SessionOverrideNote(debugUI, Field::dedicatedCaustics);
        ImGui::BeginDisabled(!pathTracing.dedicatedCaustics);
        ImGui::Checkbox("Caustic-only view", &pathTracing.causticOnlyView);
        SessionOverrideNote(debugUI, Field::causticOnlyView);
        ImGui::EndDisabled();

        if(ImGui::Checkbox("Pixel-centred caustics (experimental)", &pathTracing.pixelCentredCaustics) &&
            pathTracing.pixelCentredCaustics)
        {
            pathTracing.dedicatedCaustics = false;
        }

        SessionOverrideNote(debugUI, Field::pixelCentredCaustics);
    }

    ImGui::EndDisabled();
    DrawRenderingRuntimeStatus(debugUI);
}

void Editor::DrawAudioSettingsCategory(const DebugUI&, bool editable)
{
    AudioSettings& draft = m_projectSettingsDraft->audio;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Saved in ProjectSettings.json. Built games do not ship Steam Audio yet and run FMOD only.");
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Steam Audio");
    ImGui::BeginDisabled(!editable);
    ImGui::Checkbox("Enable Steam Audio", &draft.steamAudio);
    static constexpr const char* rayTracers[] = {"Default", "Embree"};
    DraftCombo("Ray Tracer", draft.steamAudioRayTracer, rayTracers);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Enables Steam Audio spatialization for FMOD Studio events configured with Steam Audio effects.");
    ImGui::PopTextWrapPos();

    // The runtime state comes from the audio system, never from the checkbox: Steam Audio starts (or not) once, when
    // the project opens, before its banks load, and a session never loads or unloads the plugin afterwards.
    const bool savedEnabled = m_activeProject->GetProjectSettings().settings.audio.steamAudio;
    const SteamAudioSettingsStatus status = DescribeSteamAudioSettings(savedEnabled, m_audioSystem);

    ImGui::Spacing();
    ImGui::Text("Saved setting: %s", savedEnabled ? "enabled" : "disabled");
    ImGui::TextWrapped("This session: %s", status.summary.c_str());

    if(!status.detail.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", status.detail.c_str());
        ImGui::PopTextWrapPos();
    }

    // What runs, from the audio system: it can differ from the saved choice, which a fallback never rewrites.
    const ProjectSteamAudioRayTracer savedRayTracer = m_activeProject->GetProjectSettings().settings.audio.steamAudioRayTracer;
    bool rayTracerReloadRequired = false;

    if(m_audioSystem && status.state == SteamAudioSettingsStatus::State::Active)
    {
        const Audio::SteamAudioSimulationStats stats = m_audioSystem->GetSteamAudioSimulationStats();
        ImGui::Text("Ray tracer this session: %s (requested %s)", Audio::ToString(stats.activeRayTracer), Audio::ToString(stats.requestedRayTracer));

        if(!stats.rayTracerDetail.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", stats.rayTracerDetail.c_str());
            ImGui::PopTextWrapPos();
        }

        rayTracerReloadRequired = (savedRayTracer == ProjectSteamAudioRayTracer::Embree) !=
            (stats.requestedRayTracer == Audio::SteamAudioRayTracer::Embree);
    }

    if(draft.steamAudio != savedEnabled || draft.steamAudioRayTracer != savedRayTracer)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Save, then reload the project to apply the Steam Audio setting.");
    }
    else if(rayTracerReloadRequired)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Reload the project to apply the Ray Tracer setting.");
    }
    else if(status.reloadRequired)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Reload the project to apply the Steam Audio setting.");
    }
}

void Editor::DrawRenderingRuntimeStatus(const DebugUI& debugUI)
{
    ImGui::Spacing();

    if(!ImGui::CollapsingHeader("Runtime status (not saved)"))
    {
        return;
    }

    // The renderer's publications of the last frame, as the Debug panel shows them.
    const DebugUI::RenderingStatus status = debugUI.GetRenderingStatus();
    ImGui::Text("Lighting mode: requested %s, rendering %s (%s)", GetLightingModeDisplayName(status.transition.requested),
        GetLightingModeDisplayName(status.transition.active), GetLightingModeStageLabel(status.transition.stage));

    if(status.transition.requested == RendererLightingMode::PathTracedRasterPrimaryReSTIRDI)
    {
        ImGui::Text("PSR: %s active", PsrChainName(status.psrActiveChain));

        if(status.psrRequested && !status.psrUnavailable.empty())
        {
            ImGui::TextWrapped("PSR status: %s", status.psrUnavailable.c_str());
        }

        if(!status.denoiserPreparation.empty())
        {
            ImGui::TextWrapped("Denoiser: %s", status.denoiserPreparation.c_str());
        }
    }

    ImGui::TextWrapped("DLSS: %s", status.dlssState.c_str());

    if(!status.dlssDetail.empty())
    {
        ImGui::TextWrapped("%s", status.dlssDetail.c_str());
    }

    if(status.pathTraceActiveLastFrame)
    {
        ImGui::Text("Accumulated %u samples per pixel (%.2f MiB)", status.accumulatedSamples,
            static_cast<double>(status.accumulationBytes) / (1024.0 * 1024.0));
    }

    ImGui::Text("SHARC: %s (%.1f MiB)", status.sharc.active ? "active" : "inactive",
        static_cast<double>(status.sharc.cacheBytes) / (1024.0 * 1024.0));

    if(!status.sharc.preparation.empty())
    {
        ImGui::TextWrapped("SHARC: %s", status.sharc.preparation.c_str());
    }

    ImGui::TextWrapped("Dedicated caustics: %s", status.dedicatedCausticsReason.empty() ? "off" :
        status.dedicatedCausticsReason.c_str());
}

void Editor::DrawStartupSceneSection(const IDemoScene& demoScene)
{
    const auto* manifest = m_activeProject->GetManifest();
    const auto& root = m_activeProject->GetRoot();

    ImGui::TextDisabled("Shared project settings, stored in the project's .engineproj file");
    ImGui::TextWrapped("Name: %s", manifest ? manifest->GetDisplayName().c_str() : "Legacy project");
    ImGui::TextWrapped("Location: %s", PathText(root).c_str());
    ImGui::TextWrapped("Project ID: %s", manifest ? manifest->GetProjectID().ToString().c_str() : "manifestless");

    ImGui::Separator();
    ImGui::TextUnformatted("Game Startup Scene");

    const auto configured = manifest ? manifest->GetStartupScene() : std::filesystem::path{};

    if(configured.empty())
    {
        ImGui::TextWrapped("Not set: Build Game uses the active saved scene.");
    }
    else
    {
        ImGui::TextWrapped("%s (explicit)", PathText(configured).c_str());
    }

    if(!m_startupSceneCandidatesListed)
    {
        ListStartupSceneCandidates();
    }

    const auto* activeScene = SavedActiveScene(demoScene);
    const bool editable = manifest && !m_startupSceneRequest.has_value();
    ImGui::BeginDisabled(!editable);

    const std::string preview = configured.empty() ? "Use active saved scene" : PathText(configured);

    if(ImGui::BeginCombo("Startup scene", preview.c_str()))
    {
        if(ImGui::Selectable("Use active saved scene", configured.empty()) && !configured.empty())
        {
            m_startupSceneRequest = std::filesystem::path{};
        }

        for(const auto& candidate : m_startupSceneCandidates)
        {
            const auto relative = candidate.lexically_relative(root);
            const bool selected = !configured.empty() && ProjectAssetRegistry::PathsEqual(relative, configured);

            if(ImGui::Selectable(PathText(relative).c_str(), selected) && !selected)
            {
                m_startupSceneRequest = candidate;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(!activeScene);

    if(ImGui::Button("Use Active Scene"))
    {
        m_startupSceneRequest = activeScene->GetScenePath();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(configured.empty());

    if(ImGui::Button("Clear"))
    {
        m_startupSceneRequest = std::filesystem::path{};
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Refresh List"))
    {
        ListStartupSceneCandidates();
    }

    ImGui::EndDisabled();

    if(!manifest)
    {
        ImGui::TextWrapped("This project has no manifest, so shared settings cannot be changed.");
    }

    if(!m_projectSettingsMessage.empty())
    {
        const ImVec4 errorColor(1.0f, 0.45f, 0.4f, 1.0f);
        const ImVec4 infoColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, m_projectSettingsMessageIsError ? errorColor : infoColor);
        ImGui::TextWrapped("%s", m_projectSettingsMessage.c_str());
        ImGui::PopStyleColor();
    }

}

ProjectFileAssociation::RegistryLocation Editor::FileAssociationLocation() const
{
#if defined(ENGINE_HAS_VERIFICATION)
    if(m_fileAssociationLocationForVerification)
    {
        return *m_fileAssociationLocationForVerification;
    }
#endif

    return ProjectFileAssociation::CurrentUser();
}

void Editor::RefreshFileAssociationStatus()
{
    const auto executable = ProjectFileAssociation::CurrentExecutable();
    m_fileAssociationStatus = ProjectFileAssociation::ReadStatus(FileAssociationLocation(), executable);
    m_fileAssociationEffectiveExecutable = ProjectFileAssociation::QueryEffectiveExecutable();
    m_fileAssociationStatusRead = true;
}

void Editor::DrawFileAssociation()
{
    if(!m_showFileAssociation)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 330), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("Windows File Association", &m_showFileAssociation))
    {
        ImGui::End();
        return;
    }

    if(!m_fileAssociationStatusRead)
    {
        RefreshFileAssociationStatus();
    }

    const auto executable = ProjectFileAssociation::CurrentExecutable();

    ImGui::TextDisabled("Personal Windows preference for this user; not stored in any project");
    ImGui::TextWrapped("Status: %s", ProjectFileAssociation::Describe(m_fileAssociationStatus).c_str());

    const auto effective = m_fileAssociationEffectiveExecutable.empty() ?
        std::string("none") :
        PathText(m_fileAssociationEffectiveExecutable);
    ImGui::TextWrapped("Windows currently opens .engineproj files with: %s", effective.c_str());
    ImGui::TextWrapped("This Editor: %s", PathText(executable).c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(m_fileAssociationConfirmReplace);

    if(ImGui::Button("Register .engineproj with this Editor"))
    {
        const auto result = ProjectFileAssociation::Register(FileAssociationLocation(), executable, false,
            m_fileAssociationMessage);
        m_fileAssociationConfirmReplace = result == ProjectFileAssociation::ChangeResult::NeedsConfirmation;
        RefreshFileAssociationStatus();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_fileAssociationStatus.registeredExecutable.empty() || m_fileAssociationConfirmReplace);

    const bool stale = m_fileAssociationStatus.state == ProjectFileAssociation::State::StaleRegistration;

    if(ImGui::Button(stale ? "Remove Stale Registration" : "Unregister"))
    {
        ProjectFileAssociation::Unregister(FileAssociationLocation(), executable, m_fileAssociationMessage);
        RefreshFileAssociationStatus();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Refresh"))
    {
        RefreshFileAssociationStatus();
    }

    if(m_fileAssociationConfirmReplace)
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_fileAssociationMessage.c_str());

        if(ImGui::Button("Replace for This User"))
        {
            ProjectFileAssociation::Register(FileAssociationLocation(), executable, true,
                m_fileAssociationMessage);
            m_fileAssociationConfirmReplace = false;
            RefreshFileAssociationStatus();
        }

        ImGui::SameLine();

        if(ImGui::Button("Keep Existing"))
        {
            m_fileAssociationConfirmReplace = false;
            m_fileAssociationMessage = "The existing association was kept.";
        }
    }
    else if(!m_fileAssociationMessage.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_fileAssociationMessage.c_str());
    }

    ImGui::Separator();
    ImGui::TextWrapped("Windows may keep using a program chosen in Settings > Apps > Default apps or with "
        "\"Open with > Always\". Engine never changes that protected choice; select Engine Project there if needed.");

    ImGui::End();
}

void Editor::DrawProjectOwnershipLost()
{
    constexpr const char* popupName = "Project ownership lost";

    if(m_projectOwnershipMessage.empty())
    {
        return;
    }

    if(!ImGui::IsPopupOpen(popupName))
    {
        ImGui::OpenPopup(popupName);
    }

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);

    if(ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(530);
        ImGui::TextUnformatted("The Editor can no longer verify that it exclusively owns this project's manifest. "
            "Saving, building and other project changes are disabled. Other Editors are still kept out.");
        ImGui::Separator();
        ImGui::TextUnformatted(m_projectOwnershipMessage.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Retry checks ownership again. Close Editor shuts down without writing to the "
            "project; unsaved scene changes cannot be saved in this state.");
        ImGui::PopTextWrapPos();

        if(ImGui::Button("Retry"))
        {
            m_ownershipAction = OwnershipAction::Retry;
        }

        ImGui::SameLine();

        if(ImGui::Button("Close Editor"))
        {
            m_ownershipAction = OwnershipAction::Close;
        }

        ImGui::EndPopup();
    }
}
