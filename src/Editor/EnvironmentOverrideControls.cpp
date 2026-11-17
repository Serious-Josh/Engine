// W3: a Local Level's World/Local status and its six environment override switches (Engine_World_Local_Level_Architecture §5,
// §11.3). Each switch is one undoable history entry; enabling or disabling never loses the Local Level's stored values
// (FileBackedDemoScene::SetEnvironmentOverride).
#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"

#include <imgui.h>

namespace
{
    using Group = FileBackedDemoScene::EnvironmentGroup;

    struct GroupInfo
    {
        Group group;
        const char* label;
        const char* fields;
    };

    constexpr GroupInfo Groups[] = {
        {Group::Sky, "Sky", "HDRI and environment luminance"},
        {Group::Exposure, "Exposure reference", "reference EV100"},
        {Group::Haze, "Haze", "environmental haze"},
        {Group::Mist, "Mist appearance", "mist rendering"},
        {Group::AirEnvironment, "AIR environment", "ambient temperature and relaxation"},
        {Group::Wind, "Wind", "AIR ambient velocity"}};

    const char* GroupName(int group)
    {
        for(const auto& info : Groups)
        {
            if(static_cast<int>(info.group) == group)
            {
                return info.label;
            }
        }

        return "environment";
    }
}

bool Editor::CommitEnvironmentOverride(IDemoScene& scene, int group, bool enabled, std::string* error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || owner->IsArchiveActivation() || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() || m_sceneOperationPending ||
        HasBlockingObjectCreation())
    {
        if(error)
        {
            *error = "Environment overrides can be changed in a stopped Local Level without pending edits.";
        }

        return false;
    }

    const auto* document = owner->GetSceneDocument();

    if(!document || document->kind != SceneKind::Local)
    {
        if(error)
        {
            *error = "Environment overrides belong to a Local Level.";
        }

        return false;
    }

    const bool before = FileBackedDemoScene::GetOverride(document->environmentOverrides, static_cast<Group>(group));

    if(before == enabled)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<EnvironmentOverrideEdit>(EnvironmentOverrideEdit{*m_historySession, group, before, enabled});
        const auto pathBytes = [](const std::filesystem::path& path) { return path.native().capacity() * sizeof(std::filesystem::path::value_type); };
        const size_t payload = sizeof(SceneHistoryEntry) + sizeof(EnvironmentOverrideEdit) + command->session.sceneIdentity.capacity() +
            pathBytes(command->session.projectIdentity) + pathBytes(command->session.scenePath);
        SceneHistoryEntry entry{std::move(command), m_historyEpoch, payload};
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetEnvironmentOverride(static_cast<Group>(group), enabled, error))
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

bool Editor::ReplayEnvironmentOverride(IDemoScene& scene, const EnvironmentOverrideEdit& edit, bool redo, std::string& error)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || !RenameSessionMatches(scene, edit.session))
    {
        error = "Environment override replay belongs to a stale scene session.";
        return false;
    }

    const auto* document = owner->GetSceneDocument();
    const bool expected = redo ? edit.before : edit.after;

    if(!document || FileBackedDemoScene::GetOverride(document->environmentOverrides, static_cast<Group>(edit.group)) != expected)
    {
        error = std::string("The ") + GroupName(edit.group) + " override no longer matches its history entry.";
        return false;
    }

    return owner->SetEnvironmentOverride(static_cast<Group>(edit.group), redo ? edit.after : edit.before, &error);
}

void Editor::DrawEnvironmentOverrideControls(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    // W4: a composite World preview uses the World's environment; its inspection replaces the Local Level override controls.
    if(owner && owner->GetCompositeWorld())
    {
        DrawCompositeWorldInspection(*owner);
        return;
    }

    const auto* document = owner && !owner->IsArchiveActivation() ? owner->GetSceneDocument() : nullptr;

    if(!document || document->kind != SceneKind::Local || !ImGui::CollapsingHeader("Environment (Local Level)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const auto& effective = owner->GetEffectiveEnvironment();
    const ImVec4 warning(1.0f, 0.75f, 0.3f, 1.0f);
    const char* inheritedFrom = "the Engine defaults";
    std::string worldLabel;

    switch(effective.source)
    {
    case EffectiveSceneEnvironment::Source::Unassigned:
        ImGui::TextWrapped("No World assigned. Inherited groups use the Engine defaults.");
        break;
    case EffectiveSceneEnvironment::Source::AssignedWorld:
        worldLabel = "World '" + effective.worldName + "'";
        inheritedFrom = worldLabel.c_str();
        ImGui::TextWrapped("Local Level in %s. Inherited groups use the World's values; the World's entities are not loaded.",
            worldLabel.c_str());
        // The sun is an entity: a World's Directional Lights belong to the World and are not part of this standalone preview.
        ImGui::TextColored(warning, owner->WorldHasDirectionalLights() ?
            "The World's Directional Lights (sun) are not shown in this standalone preview." :
            "World-owned directional lighting is not shown in this standalone preview.");
        break;
    case EffectiveSceneEnvironment::Source::InvalidWorld:
        ImGui::TextColored(warning, "Owning World unavailable: %s Inherited groups use the Engine defaults.",
            effective.worldProblem.c_str());
        break;
    case EffectiveSceneEnvironment::Source::World:
        break;
    }

    const bool locked = IsSceneEditingLocked() || m_sceneOperationPending;
    ImGui::BeginDisabled(locked);

    for(const auto& info : Groups)
    {
        bool enabled = FileBackedDemoScene::GetOverride(document->environmentOverrides, info.group);
        ImGui::PushID(static_cast<int>(info.group));

        if(ImGui::Checkbox(info.label, &enabled))
        {
            std::string error;
            m_environmentOverrideStatus = CommitEnvironmentOverride(scene, static_cast<int>(info.group), enabled, &error) ? std::string{} : error;
        }

        ImGui::SameLine();
        ImGui::TextDisabled(enabled ? "(own %s)" : "(%s from %s)", info.fields, inheritedFrom);
        ImGui::PopID();
    }

    // Wind: the Local Level's own ambient velocity while its override is enabled (the rest of the AIR simulation stays as stored and,
    // when assigned, is the World's). One undoable AIR simulation edit that changes only the wind.
    if(document->environmentOverrides.wind)
    {
        const AirSimulationSettings stored = document->airSimulation.value_or(AirSimulationSettings{});
        float wind[3] = {stored.ambientVelocity.x, stored.ambientVelocity.y, stored.ambientVelocity.z};
        const float limit = AirSimulationSettings::MaximumAmbientSpeed;
        ImGui::DragFloat3("Wind (m/s)", wind, 0.05f, -limit, limit, "%.2f");

        if(ImGui::IsItemDeactivatedAfterEdit())
        {
            AirSimulationSettings edited = stored;
            edited.ambientVelocity = {wind[0], wind[1], wind[2]};
            std::string error;
            m_environmentOverrideStatus = CommitAirSimulation(scene, document->airSimulation, edited, &error) ? std::string{} : error;
        }
    }

    ImGui::EndDisabled();

    if(!m_environmentOverrideStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_environmentOverrideStatus.c_str());
    }
}

bool Editor::IsEnvironmentGroupInherited(IDemoScene& scene, int group)
{
    const auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const auto* document = owner && !owner->IsArchiveActivation() ? owner->GetSceneDocument() : nullptr;
    return document && document->kind == SceneKind::Local &&
        !FileBackedDemoScene::GetOverride(document->environmentOverrides, static_cast<Group>(group));
}
