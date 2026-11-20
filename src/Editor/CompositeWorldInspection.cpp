// W4: read-only inspection of a World opened as a composite preview: the World, its Local Levels and placements, the selected
// entity's owning level, the frame-wide environment and sun (approved decisions A1/B1) and the shared light count. Nothing here
// edits; World editing arrives in W5.
#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/LightComponent.h"

#include <imgui.h>

#include <numbers>

namespace
{
    std::string LevelName(const CompositeWorld& composite, const std::string& levelID)
    {
        if(levelID == composite.worldID)
        {
            return "World '" + composite.worldName + "'";
        }

        for(const auto& level : composite.levels)
        {
            if(level.id == levelID)
            {
                return "Local Level '" + level.name + "'";
            }
        }

        return levelID;
    }

    // The runtime id of the selected object, through the committed mapping (runtime slots are not document indices).
    const std::string* SelectedRuntimeID(const SceneObjectMapping& mapping, std::size_t selected)
    {
        for(const auto& [id, index] : mapping.idToRuntimeIndex)
        {
            if(index == selected)
            {
                return &id;
            }
        }

        return nullptr;
    }
}

void Editor::DrawCompositeWorldInspection(FileBackedDemoScene& owner)
{
    const CompositeWorld* composite = owner.GetCompositeWorld();

    if(!composite || !ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const ImVec4 note(1.0f, 0.75f, 0.3f, 1.0f);
    ImGui::Text("World: %s", composite->worldName.c_str());
    ImGui::TextDisabled("%s", composite->worldRelativePath.generic_string().c_str());
    DrawCompositeDocuments(owner, *composite);

    // Frame-wide policies.
    ImGui::SeparatorText("Frame");
    ImGui::TextWrapped("Environment: the World's (sky, exposure, haze, mist, AIR environment, wind). Local Level overrides are not "
        "applied in a World preview.");

    if(composite->sun)
    {
        ImGui::TextWrapped("Sun: %s, entity %s", LevelName(*composite, composite->sun->levelID).c_str(),
            composite->sun->originalID.c_str());
    }
    else
    {
        ImGui::TextWrapped("Sun: none (no enabled Directional Light in the World or its Local Levels).");
    }

    if(!composite->suppressedDirectionalLights.empty())
    {
        ImGui::TextColored(note, "%zu other enabled Directional Light(s) do not illuminate this preview (one sun per frame).",
            composite->suppressedDirectionalLights.size());
    }

    ImGui::Text("Light components: %zu (shared by all levels)", composite->lightCount);

    if(!composite->unverifiedScenes.empty())
    {
        ImGui::TextColored(note, "Membership not verified against %zu unreadable project scene(s); one of them could also list these "
            "Local Levels.", composite->unverifiedScenes.size());

        if(ImGui::IsItemHovered())
        {
            std::string problems;

            for(const auto& problem : composite->unverifiedScenes)
            {
                problems += problem + "\n";
            }

            ImGui::SetTooltip("%s", problems.c_str());
        }
    }
    ImGui::TextDisabled("AIR: World simulation settings only; Local voxel bakes are not combined (no solids).");

    // The selected entity's owner.
    ImGui::SeparatorText("Selection");
    const std::string* selectedID = m_selectedObject != InvalidObjectSelection && owner.m_runtime ?
        SelectedRuntimeID(owner.m_runtime->GetObjectMapping(), m_selectedObject) : nullptr;
    const CompositeWorld::Entity* entity = selectedID ? composite->Find(*selectedID) : nullptr;

    if(!entity)
    {
        ImGui::TextDisabled("No entity selected.");
    }
    else if(entity->kind == CompositeWorld::EntityKind::LevelRoot)
    {
        ImGui::TextWrapped("%s placement root (synthetic, not saved)", LevelName(*composite, entity->levelID).c_str());
    }
    else
    {
        ImGui::TextWrapped("Owned by %s; entity ID %s", LevelName(*composite, entity->levelID).c_str(), entity->originalID.c_str());
    }

    // Local Levels in membership order.
    ImGui::SeparatorText("Local Levels");
    constexpr float degrees = 180.0f / std::numbers::pi_v<float>;

    for(const auto& level : composite->levels)
    {
        ImGui::PushID(level.id.c_str());

        if(ImGui::TreeNode("level", "%s (%zu entities, %zu lights)", level.name.c_str(), level.entityCount, level.lightCount))
        {
            const auto& placement = level.placement;
            ImGui::TextDisabled("%s", level.relativePath.generic_string().c_str());
            ImGui::Text("Position: %.3f, %.3f, %.3f m", placement.position.x, placement.position.y, placement.position.z);
            ImGui::Text("Rotation: %.2f, %.2f, %.2f deg", placement.rotation.x * degrees, placement.rotation.y * degrees,
                placement.rotation.z * degrees);

            if(level.differencesFromWorld.empty())
            {
                ImGui::TextDisabled("Environment: same as the World's.");
            }
            else
            {
                ImGui::TextWrapped("Standalone, this Local Level's own %s differ from the World's (not applied in this preview).",
                    level.differencesFromWorld.c_str());
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }
}

// W5: the editing context (where new entities go), each source document's unsaved state, and Save / Save All. Dirty state needs a
// projection of the whole composite, so it is refreshed every 30 frames, not every frame.
void Editor::DrawCompositeDocuments(FileBackedDemoScene& owner, const CompositeWorld& composite)
{
    constexpr int refreshFrames = 30;

    if(m_compositeDirtyCountdown-- <= 0)
    {
        m_compositeDirtyCountdown = refreshFrames;
        m_compositeDirty.clear();
        owner.GetCompositeDirtyDocuments(m_compositeDirty, &m_compositeSaveStatus);
    }

    const auto dirty = [&](std::size_t index)
    {
        return index < m_compositeDirty.size() && m_compositeDirty[index];
    };
    const auto label = [&](std::size_t index)
    {
        return index == 0 ? "World '" + composite.worldName + "'" : "Local Level '" + composite.levels[index - 1].name + "'";
    };

    ImGui::TextDisabled("Edits go to each entity's owning document. Select a Local Level node to move or rotate it (no scale).");

    // New entities join the editing context (an entity's duplicate stays in its source's level).
    const std::string& editing = owner.GetEditingLevel();
    std::string preview = "World '" + composite.worldName + "'";

    for(const auto& level : composite.levels)
    {
        if(level.id == editing)
        {
            preview = "Local Level '" + level.name + "'";
        }
    }

    if(ImGui::BeginCombo("New entities in", preview.c_str()))
    {
        if(ImGui::Selectable(("World '" + composite.worldName + "'").c_str(), editing == composite.worldID))
        {
            owner.SetEditingLevel(composite.worldID);
        }

        for(const auto& level : composite.levels)
        {
            if(ImGui::Selectable(("Local Level '" + level.name + "'").c_str(), editing == level.id))
            {
                owner.SetEditingLevel(level.id);
            }
        }

        ImGui::EndCombo();
    }

    // Unsaved documents, each with its own Save; Save All writes them together or not at all.
    bool anyDirty = false;

    for(std::size_t index = 0; index < 1 + composite.levels.size(); ++index)
    {
        if(!dirty(index))
        {
            continue;
        }

        anyDirty = true;
        ImGui::PushID(static_cast<int>(index));
        ImGui::Text("* %s", label(index).c_str());
        ImGui::SameLine();

        if(ImGui::SmallButton("Save"))
        {
            m_compositeSaveRequest = std::optional<std::size_t>(index);
            m_compositeDirtyCountdown = 0;
        }

        ImGui::PopID();
    }

    if(!anyDirty)
    {
        ImGui::TextDisabled("All documents saved.");
    }

    ImGui::BeginDisabled(!anyDirty);

    if(ImGui::Button("Save All"))
    {
        m_compositeSaveRequest = std::optional<std::size_t>{};
        m_compositeDirtyCountdown = 0;
    }

    ImGui::EndDisabled();

    if(!m_compositeSaveStatus.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_compositeSaveStatus.c_str());
    }
}
