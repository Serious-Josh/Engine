#include "Editor/SceneMembershipPanel.h"

#include "Editor/MistAppearanceFields.h"
#include "Project/ProjectPaths.h"
#include "Renderer/Renderer.h"
#include "assets/ProjectAssetRegistry.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <numbers>

namespace
{
    namespace fs = std::filesystem;
    using ProjectSceneMembership::SceneInfo;

    constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

    std::string UTF8(const fs::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    template<std::size_t Size>
    void SetText(std::array<char, Size>& buffer, const std::string& text)
    {
        buffer.fill(0);
        std::memcpy(buffer.data(), text.data(), (std::min)(text.size(), Size - 1));
    }

    // A file name for a scene name: characters Windows forbids become '_'; empty names are refused by the caller.
    std::string FileStem(const char* name)
    {
        std::string stem(name);

        for(auto& character : stem)
        {
            if(std::strchr("\\/:*?\"<>|", character) || static_cast<unsigned char>(character) < 32)
            {
                character = '_';
            }
        }

        while(!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        {
            stem.pop_back();
        }

        return stem;
    }

    ScenePlacement ToPlacement(const std::array<float, 6>& draft)
    {
        return {{draft[0], draft[1], draft[2]},
            {draft[3] / DegreesPerRadian, draft[4] / DegreesPerRadian, draft[5] / DegreesPerRadian}};
    }

    std::array<float, 6> ToDraft(const ScenePlacement& placement)
    {
        return {placement.position.x, placement.position.y, placement.position.z, placement.rotation.x * DegreesPerRadian,
            placement.rotation.y * DegreesPerRadian, placement.rotation.z * DegreesPerRadian};
    }
}

void SceneMembershipPanel::Refresh(const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    if(!m_stale && m_root == paths.GetRoot())
    {
        return;
    }

    m_root = paths.GetRoot();
    m_stale = false;
    m_problems.clear();
    ProjectSceneMembership::ReadScenes(paths, registry, m_scenes, m_problems);

    for(const auto& problem : ProjectSceneMembership::ValidateProject(m_scenes))
    {
        m_problems.push_back(problem);
    }

    m_placementDrafts.clear();
    m_worldEnvironmentDraft.reset();
    m_worldBuiltinEnvironmentDraft.clear();
}

const SceneInfo* SceneMembershipPanel::FindByPath(const fs::path& absolutePath) const
{
    const auto relative = Relative(absolutePath);

    for(const auto& scene : m_scenes)
    {
        if(ProjectAssetRegistry::PathsEqual(scene.relativePath, relative))
        {
            return &scene;
        }
    }

    return nullptr;
}

const SceneInfo* SceneMembershipPanel::FindByID(const std::string& id) const
{
    for(const auto& scene : m_scenes)
    {
        if(scene.id.ToString() == id)
        {
            return &scene;
        }
    }

    return nullptr;
}

fs::path SceneMembershipPanel::Relative(const fs::path& absolutePath) const
{
    return absolutePath.lexically_normal().lexically_relative(m_root.lexically_normal());
}

std::string SceneMembershipPanel::Describe(const fs::path& absolutePath) const
{
    const auto* scene = FindByPath(absolutePath);

    if(!scene)
    {
        return "Scene (not yet registered; reopen the project to register it)";
    }

    if(scene->kind == SceneKind::World)
    {
        return "World: " + std::to_string(scene->members.size()) + " Local Level" + (scene->members.size() == 1 ? "" : "s");
    }

    if(scene->owningWorld.empty())
    {
        return "Local Level: no World assigned";
    }

    const auto* world = FindByID(scene->owningWorld);
    return "Local Level in World '" + (world ? world->name : scene->owningWorld) + "'";
}

void SceneMembershipPanel::DrawSceneContextItems(const fs::path& absolutePath, bool enabled)
{
    const auto* scene = FindByPath(absolutePath);

    if(!scene)
    {
        return;
    }

    enabled = enabled && !m_pending;
    ImGui::Separator();

    if(scene->kind == SceneKind::World)
    {
        if(ImGui::MenuItem("World Membership...", nullptr, false, enabled))
        {
            m_world = scene->id;
            m_showWorld = true;
            m_assignLocal = -1;
            m_addLocalName.fill(0);
            m_placementDrafts.clear();
        }

        return;
    }

    if(scene->owningWorld.empty())
    {
        if(ImGui::MenuItem("Create World with This Local Level...", nullptr, false, enabled))
        {
            m_createDirectory = absolutePath.parent_path();
            SetText(m_worldName, scene->name + " World");
            m_newLocalName.fill(0);
            m_initialLocal = -1;
            int index = 0;

            for(const auto& candidate : m_scenes)
            {
                if(candidate.kind == SceneKind::Local && candidate.owningWorld.empty())
                {
                    if(candidate.id == scene->id)
                    {
                        m_initialLocal = index;
                    }

                    ++index;
                }
            }

            m_environment = 0;
            m_openCreateWorld = true;
        }
    }
    else if(ImGui::MenuItem("Unassign from World", nullptr, false, enabled))
    {
        Request request;
        request.action = Action::Unassign;
        request.local = scene->id;
        Submit(request);
    }

    if(ImGui::MenuItem("Duplicate Local Level...", nullptr, false, enabled))
    {
        m_duplicateSource = scene->id;
        m_duplicateDirectory = absolutePath.parent_path();
        SetText(m_duplicateName, scene->name + " (Copy)");
        m_openDuplicate = true;
    }
}

void SceneMembershipPanel::DrawCreateWorldMenuItem(const fs::path& directory, bool enabled)
{
    if(ImGui::MenuItem("World...", nullptr, false, enabled && !m_pending && !m_root.empty()))
    {
        m_createDirectory = directory;
        SetText(m_worldName, "New World");
        SetText(m_newLocalName, "New Local Level");
        m_initialLocal = -1;
        m_environment = 1;
        m_openCreateWorld = true;
    }
}

void SceneMembershipPanel::Submit(Request request)
{
    m_request = std::move(request);
    m_pending = true;
    m_status.clear();
}

std::optional<SceneMembershipPanel::Request> SceneMembershipPanel::TakeRequest()
{
    return std::exchange(m_request, std::nullopt);
}

void SceneMembershipPanel::OnCompleted(const std::string& error, const std::string& message)
{
    m_pending = false;
    m_statusIsError = !error.empty();
    m_status = error.empty() ? message : error;
    m_stale = true;
}

void SceneMembershipPanel::Draw(bool locked)
{
    DrawCreateWorldModal(locked);
    DrawDuplicateModal(locked);
    DrawWorldWindow(locked);
}

void SceneMembershipPanel::DrawCreateWorldModal(bool locked)
{
    if(m_openCreateWorld)
    {
        ImGui::OpenPopup("Create World");
        m_openCreateWorld = false;
    }

    if(!ImGui::BeginPopupModal("Create World", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    std::vector<const SceneInfo*> unassigned;

    for(const auto& scene : m_scenes)
    {
        if(scene.kind == SceneKind::Local && scene.owningWorld.empty())
        {
            unassigned.push_back(&scene);
        }
    }

    ImGui::InputText("World name", m_worldName.data(), m_worldName.size());
    ImGui::TextUnformatted("Initial Local Level (a World always has at least one):");
    const std::string preview = m_initialLocal >= 0 && m_initialLocal < static_cast<int>(unassigned.size()) ?
        unassigned[m_initialLocal]->name : std::string("New Local Level");

    if(ImGui::BeginCombo("Local Level", preview.c_str()))
    {
        if(ImGui::Selectable("New Local Level", m_initialLocal < 0))
        {
            m_initialLocal = -1;
        }

        for(int index = 0; index < static_cast<int>(unassigned.size()); ++index)
        {
            const std::string label = unassigned[index]->name + "  (" + UTF8(unassigned[index]->relativePath) + ")";

            if(ImGui::Selectable(label.c_str(), m_initialLocal == index))
            {
                m_initialLocal = index;
            }
        }

        ImGui::EndCombo();
    }

    if(m_initialLocal < 0)
    {
        ImGui::InputText("New Local Level name", m_newLocalName.data(), m_newLocalName.size());
    }

    ImGui::TextUnformatted("World environment:");
    ImGui::RadioButton("Import from the Local Level", &m_environment, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Default", &m_environment, 1);

    const std::string worldStem = FileStem(m_worldName.data());
    const std::string localStem = FileStem(m_newLocalName.data());
    const bool named = !worldStem.empty() && (m_initialLocal >= 0 || !localStem.empty()) && (m_initialLocal >= 0 || worldStem != localStem);
    const auto directory = Relative(m_createDirectory);
    ImGui::TextDisabled("Files: %s%s", UTF8(directory / (worldStem + ".scene")).c_str(),
        m_initialLocal < 0 ? (", " + UTF8(directory / (localStem + ".scene"))).c_str() : "");

    if(locked)
    {
        ImGui::TextWrapped("Finish or discard pending edits and scene operations first.");
    }

    ImGui::BeginDisabled(locked || !named || m_pending);

    if(ImGui::Button("Create"))
    {
        Request request;
        request.action = Action::CreateWorld;
        request.worldPath = directory / (worldStem + ".scene");
        request.worldName = m_worldName.data();
        request.environment = m_environment == 0 ? ProjectSceneMembership::EnvironmentSource::Import :
            ProjectSceneMembership::EnvironmentSource::Default;

        if(m_initialLocal >= 0 && m_initialLocal < static_cast<int>(unassigned.size()))
        {
            request.local = unassigned[m_initialLocal]->id;
        }
        else
        {
            request.localPath = directory / (localStem + ".scene");
            request.localName = m_newLocalName.data();
        }

        Submit(request);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void SceneMembershipPanel::DrawDuplicateModal(bool locked)
{
    if(m_openDuplicate)
    {
        ImGui::OpenPopup("Duplicate Local Level");
        m_openDuplicate = false;
    }

    if(!ImGui::BeginPopupModal("Duplicate Local Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextWrapped("The duplicate gets new scene and entity identities and is not assigned to any World.");
    ImGui::InputText("Name", m_duplicateName.data(), m_duplicateName.size());
    const std::string stem = FileStem(m_duplicateName.data());
    const auto path = Relative(m_duplicateDirectory) / (stem + ".scene");
    ImGui::TextDisabled("File: %s", UTF8(path).c_str());
    ImGui::BeginDisabled(locked || stem.empty() || m_pending || !m_duplicateSource);

    if(ImGui::Button("Duplicate"))
    {
        Request request;
        request.action = Action::Duplicate;
        request.local = m_duplicateSource;
        request.localPath = path;
        request.localName = m_duplicateName.data();
        Submit(request);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if(ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void SceneMembershipPanel::DrawWorldWindow(bool locked)
{
    if(!m_showWorld || !m_world)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);

    if(!ImGui::Begin("World Membership", &m_showWorld))
    {
        ImGui::End();
        return;
    }

    const auto* world = FindByID(m_world->ToString());

    if(!world || world->kind != SceneKind::World)
    {
        ImGui::TextWrapped("The World is no longer available.");
        ImGui::End();
        return;
    }

    ImGui::Text("World '%s'  (%s)", world->name.c_str(), UTF8(world->relativePath).c_str());
    ImGui::TextDisabled("Placement is the World's: translation and rotation (degrees), no scale. Local Level contents are unchanged.");

    if(!m_status.empty())
    {
        ImGui::TextColored(m_statusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", m_status.c_str());
    }

    for(const auto& problem : m_problems)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", problem.c_str());
    }

    if(m_placementDrafts.size() != world->members.size())
    {
        m_placementDrafts.clear();

        for(const auto& member : world->members)
        {
            m_placementDrafts.push_back(ToDraft(member.placement));
        }
    }

    ImGui::Separator();
    ImGui::BeginDisabled(locked || m_pending);

    for(std::size_t index = 0; index < world->members.size(); ++index)
    {
        const auto& member = world->members[index];
        const auto* local = FindByID(member.level);
        ImGui::PushID(static_cast<int>(index));
        ImGui::Text("%s  (%s)", local ? local->name.c_str() : member.level.c_str(),
            local ? UTF8(local->relativePath).c_str() : "missing");
        auto& draft = m_placementDrafts[index];
        ImGui::DragFloat3("Position", draft.data(), 0.05f);
        ImGui::DragFloat3("Rotation", draft.data() + 3, 0.5f);

        if(ImGui::Button("Apply Placement"))
        {
            Request request;
            request.action = Action::Placement;
            request.world = world->id;
            request.local = *AssetID::Parse(member.level);
            request.placement = ToPlacement(draft);
            Submit(request);
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(world->members.size() <= 1);

        if(ImGui::Button("Unassign"))
        {
            Request request;
            request.action = Action::Unassign;
            request.local = *AssetID::Parse(member.level);
            Submit(request);
        }

        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::PopID();
    }

    std::vector<const SceneInfo*> unassigned;

    for(const auto& scene : m_scenes)
    {
        if(scene.kind == SceneKind::Local && scene.owningWorld.empty())
        {
            unassigned.push_back(&scene);
        }
    }

    const std::string preview = m_assignLocal >= 0 && m_assignLocal < static_cast<int>(unassigned.size()) ?
        unassigned[m_assignLocal]->name : std::string("Choose an unassigned Local Level");

    if(ImGui::BeginCombo("##assign", preview.c_str()))
    {
        for(int index = 0; index < static_cast<int>(unassigned.size()); ++index)
        {
            if(ImGui::Selectable((unassigned[index]->name + "  (" + UTF8(unassigned[index]->relativePath) + ")").c_str(),
                m_assignLocal == index))
            {
                m_assignLocal = index;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(m_assignLocal < 0 || m_assignLocal >= static_cast<int>(unassigned.size()));

    if(ImGui::Button("Assign"))
    {
        Request request;
        request.action = Action::Assign;
        request.world = world->id;
        request.local = unassigned[m_assignLocal]->id;
        Submit(request);
        m_assignLocal = -1;
    }

    ImGui::EndDisabled();
    ImGui::InputText("##newlocal", m_addLocalName.data(), m_addLocalName.size());
    ImGui::SameLine();
    const std::string stem = FileStem(m_addLocalName.data());
    ImGui::BeginDisabled(stem.empty());

    if(ImGui::Button("Create and Assign New Local Level"))
    {
        Request request;
        request.action = Action::CreateAndAssign;
        request.world = world->id;
        request.localPath = world->relativePath.parent_path() / (stem + ".scene");
        request.localName = m_addLocalName.data();
        Submit(request);
        m_addLocalName.fill(0);
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();
    DrawWorldEnvironment(*world, locked);

    if(locked)
    {
        ImGui::TextWrapped("Finish or discard pending edits and scene operations first.");
    }

    ImGui::End();
}

void SceneMembershipPanel::DrawWorldEnvironment(const SceneInfo& world, bool locked)
{
    if(!ImGui::CollapsingHeader("World environment"))
    {
        return;
    }

    ImGui::TextDisabled("World-owned defaults: Local Levels inherit every group they do not override. Local documents are not changed.");

    if(!m_worldEnvironmentDraft)
    {
        m_worldEnvironmentDraft = world.environment;
    }

    auto& draft = *m_worldEnvironmentDraft;
    ImGui::BeginDisabled(locked || m_pending);
    DrawWorldSky(draft);
    ImGui::InputFloat("Reference exposure (EV100)", &draft.lightingSettings.referenceEV100, 0.5f, 1.0f, "%.2f");

    bool haze = draft.environmentalHaze && draft.environmentalHaze->enabled;

    if(ImGui::Checkbox("Haze enabled", &haze))
    {
        auto settings = draft.environmentalHaze.value_or(EnvironmentalHazeSettings{});
        settings.enabled = haze;
        draft.environmentalHaze = settings;
    }

    if(draft.environmentalHaze)
    {
        ImGui::InputFloat("Haze attenuation distance (m)", &draft.environmentalHaze->attenuationDistance, 1.0f, 10.0f, "%.1f");
    }

    auto airEnvironment = draft.airEnvironment.value_or(AirEnvironmentSettings{});

    if(ImGui::InputFloat("Ambient temperature (K)", &airEnvironment.ambientTemperature, 1.0f, 10.0f, "%.2f") |
        ImGui::InputFloat("Thermal relaxation (s)", &airEnvironment.relaxationTime, 1.0f, 10.0f, "%.2f"))
    {
        draft.airEnvironment = airEnvironment;
    }

    auto simulation = draft.airSimulation.value_or(AirSimulationSettings{});
    float wind[3] = {simulation.ambientVelocity.x, simulation.ambientVelocity.y, simulation.ambientVelocity.z};
    const bool simulationEdited = ImGui::Checkbox("AIR simulation enabled", &simulation.enabled) |
        ImGui::DragFloat3("Wind (m/s)", wind, 0.05f, -AirSimulationSettings::MaximumAmbientSpeed, AirSimulationSettings::MaximumAmbientSpeed, "%.2f");

    if(simulationEdited)
    {
        simulation.ambientVelocity = {wind[0], wind[1], wind[2]};
        draft.airSimulation = simulation;
    }

    DrawWorldMist(draft);

    // Detailed haze and AIR settings: copy them from a Local Level edited with the full controls.
    std::vector<const SceneInfo*> locals;

    for(const auto& scene : m_scenes)
    {
        if(scene.kind == SceneKind::Local)
        {
            locals.push_back(&scene);
        }
    }

    const std::string preview = m_copyEnvironmentFrom >= 0 && m_copyEnvironmentFrom < static_cast<int>(locals.size()) ?
        locals[m_copyEnvironmentFrom]->name : std::string("Copy environment from a Local Level...");

    if(ImGui::BeginCombo("##copyenvironment", preview.c_str()))
    {
        for(int index = 0; index < static_cast<int>(locals.size()); ++index)
        {
            if(ImGui::Selectable((locals[index]->name + "  (" + UTF8(locals[index]->relativePath) + ")").c_str(), m_copyEnvironmentFrom == index))
            {
                m_copyEnvironmentFrom = index;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(m_copyEnvironmentFrom < 0 || m_copyEnvironmentFrom >= static_cast<int>(locals.size()));

    if(ImGui::Button("Copy"))
    {
        // The Local Level's own environment as a standalone scene (its overridden groups; Engine defaults for the rest).
        draft = locals[m_copyEnvironmentFrom]->environment;
        m_worldBuiltinEnvironmentDraft.clear();
        m_copyEnvironmentFrom = -1;
    }

    ImGui::EndDisabled();
    const bool changed = !DescribeEnvironmentDifferences(world.environment, draft).empty() || !m_worldBuiltinEnvironmentDraft.empty();

    ImGui::BeginDisabled(!changed);

    if(ImGui::Button("Apply World environment"))
    {
        Request request;
        request.action = Action::WorldEnvironment;
        request.world = world.id;
        request.expectedEnvironment = world.environment;
        request.worldEnvironment = draft;
        request.worldBuiltinEnvironment = m_worldBuiltinEnvironmentDraft;
        Submit(request);
    }

    ImGui::SameLine();

    if(ImGui::Button("Revert"))
    {
        m_worldEnvironmentDraft = world.environment;
        m_worldBuiltinEnvironmentDraft.clear();
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void SceneMembershipPanel::SyncEnvironmentChoices(const std::vector<EnvironmentAsset>& environments, const fs::path& builtinRoot)
{
    const auto matches = [&](const EnvironmentAsset& environment, const EnvironmentChoice& choice)
    {
        return environment.displayName == choice.displayName && environment.reference == choice.reference &&
            (environment.relativePath.empty() ? choice.builtinSource.empty() : choice.builtinSource == builtinRoot / environment.relativePath);
    };

    if(std::ranges::equal(environments, m_environmentChoices, matches))
    {
        return;
    }

    m_environmentChoices.clear();
    m_environmentChoices.reserve(environments.size());

    for(const auto& environment : environments)
    {
        const fs::path builtinSource = environment.relativePath.empty() ? fs::path{} : builtinRoot / environment.relativePath;
        m_environmentChoices.push_back({environment.displayName, environment.reference, builtinSource});
    }
}

// The World's sky reference. A built-in HDRI is imported into the project when the edit is applied (as when a Local Level
// assigns one), so the World stores a stable project reference.
void SceneMembershipPanel::DrawWorldSky(EffectiveSceneEnvironment& draft)
{
    const EnvironmentChoice* shown = nullptr;

    // A pending built-in choice is matched by its file; otherwise the stored reference is matched among the project choices.
    const bool builtinPending = !m_worldBuiltinEnvironmentDraft.empty();

    for(const auto& choice : m_environmentChoices)
    {
        const bool matchesBuiltin = builtinPending && choice.builtinSource == m_worldBuiltinEnvironmentDraft;
        const bool matchesReference = !builtinPending && choice.builtinSource.empty() && choice.reference == draft.environmentReference;

        if(matchesBuiltin || matchesReference)
        {
            shown = &choice;
        }
    }

    // A reference that no listed HDRI matches (a missing asset, or a list not yet published) is shown as stored.
    std::string preview = draft.environmentReference.empty() ? std::string("Neutral") : draft.environmentReference;

    if(shown)
    {
        preview = shown->displayName;
    }

    // Physical sky S4 (§4.12.1): the World's sky is shown here and edited only in Level > Sky with the World open, so no second path
    // writes it.
    const SkyType type = EffectiveSkyType(draft);
    ImGui::Text("World sky: %s", type == SkyType::Physical ? "Physically Based Sky" :
        (type == SkyType::Hdri ? ("HDRI Sky, " + preview).c_str() : "Neutral"));
    ImGui::TextDisabled("Edit it in Level > Sky with the World open.");
}

// The World's mist appearance: absent until enabled, then every field of the Local Level mist panel.
void SceneMembershipPanel::DrawWorldMist(EffectiveSceneEnvironment& draft)
{
    bool authored = draft.mistRendering.has_value();

    if(ImGui::Checkbox("World mist appearance", &authored))
    {
        draft.mistRendering = authored ? std::optional<MistRenderSettings>(MistRenderSettings{}) : std::nullopt;
    }

    if(ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off: inheriting Local Levels use the Engine's default mist appearance.");
    }

    if(!draft.mistRendering || !ImGui::TreeNode("Mist appearance"))
    {
        return;
    }

    MistRenderSettings settings = *draft.mistRendering;

    if(DrawMistAppearanceFields(settings).changed)
    {
        draft.mistRendering = SanitizeMistRenderSettings(settings);
    }

    ImGui::TreePop();
}
