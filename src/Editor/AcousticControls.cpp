#include "Editor/Editor.h"

#include "DemoContent/FileBackedDemoScene.h"
#include "Engine/AcousticGeometry.h"
#include "assets/AcousticMaterial.h"
#include "assets/ProjectAssetRegistry.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace
{
    const ImVec4 WarningColor(1.0f, 0.65f, 0.3f, 1.0f);

    std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        return text;
    }
}

size_t Editor::AcousticEditPayloadBytes(const AcousticSettingsEdit& edit)
{
    const auto settingsBytes = [](const std::optional<AcousticSettings>& settings)
    {
        size_t bytes = 0;

        if(settings)
        {
            for(const auto& [key, reference] : settings->submeshMaterials)
            {
                bytes += key.capacity() + reference.capacity() + 64;
            }
        }

        return bytes;
    };

    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };

    return sizeof(SceneHistoryEntry) + sizeof(AcousticSettingsEdit) + edit.objectID.capacity() +
        edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath) + settingsBytes(edit.before) + settingsBytes(edit.after);
}

// One complete replacement of the object's acoustic settings per history entry.
bool Editor::CommitObjectAcoustics(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<AcousticSettings>& before, const std::optional<AcousticSettings>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsSceneEditingLocked() || !owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        HasPendingSceneHistoryOperation() || m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_acousticEditStatus = "Acoustics can be edited in a stopped scene without pending edits.";
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_acousticEditStatus = "The acoustics inspection target is stale or blocked.";
        return false;
    }

    if(after && !ValidateAcousticSettings(*after, &m_acousticEditStatus))
    {
        return false;
    }

    if(before == after)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<AcousticSettingsEdit>(AcousticSettingsEdit{*m_historySession, selection.objectID, before, after});
        const size_t bytes = AcousticEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_acousticEditStatus = "This edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectAcoustics(selection, before, after, &m_acousticEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_acousticEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_acousticEditStatus = exception.what();
        return false;
    }
}

// A reference dropped on a submesh field. Anything but an acoustic material of this project is refused, unchanged.
bool Editor::AssignAcousticMaterialReference(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<AcousticSettings>& before, const std::string& submeshKey, std::string_view reference,
    const ProjectAssetRegistry& registry)
{
    const auto id = AssetID::ParseReference(std::string(reference));
    const AssetRecord* record = id ? registry.Find(*id) : nullptr;

    if(!record || record->type != AssetType::AcousticMaterial)
    {
        m_acousticEditStatus = "Only an Acoustic Material can be assigned here.";
        return false;
    }

    AcousticSettings after = before.value_or(AcousticSettings{});
    after.submeshMaterials[submeshKey] = std::string(reference);
    return CommitObjectAcoustics(scene, selection, before, after);
}

void Editor::DrawAcousticMaterialPicker(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<AcousticSettings>& before, const std::string& submeshKey, const ProjectAssetRegistry& registry)
{
    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_Appearing);

    if(!ImGui::BeginPopup("AcousticMaterialPicker"))
    {
        return;
    }

    if(ImGui::IsWindowAppearing())
    {
        m_acousticMaterialSearch.fill(0);
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Search", "Search acoustic materials", m_acousticMaterialSearch.data(), m_acousticMaterialSearch.size());
    const std::string search = Lowercase(m_acousticMaterialSearch.data());
    std::optional<std::string> chosen;
    bool cleared = false;

    if(ImGui::Selectable("None (default material)"))
    {
        cleared = true;
    }

    ImGui::SeparatorText("Built-in (read-only)");

    for(const std::string& reference : AcousticMaterials::BuiltinReferences())
    {
        const std::string name = AcousticMaterials::BuiltinDisplayName(reference);

        if(Lowercase(name).find(search) != std::string::npos && ImGui::Selectable(name.c_str()))
        {
            chosen = reference;
        }
    }

    ImGui::SeparatorText("Project");
    size_t projectEntries = 0;

    for(const AssetRecord& record : registry.GetAssets(AssetType::AcousticMaterial))
    {
        const std::string name = record.relativePath.stem().string();
        const std::string path = record.relativePath.generic_string();

        if(Lowercase(path).find(search) == std::string::npos)
        {
            continue;
        }

        ++projectEntries;
        const std::string label = name + "##" + record.id.ToReference();

        if(ImGui::Selectable(label.c_str()))
        {
            chosen = record.id.ToReference();
        }

        if(ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", path.c_str());
        }
    }

    if(projectEntries == 0)
    {
        ImGui::TextDisabled("Asset Browser: Create > Material > Acoustic Material");
    }

    if(chosen || cleared)
    {
        AcousticSettings after = before.value_or(AcousticSettings{});

        if(cleared)
        {
            after.submeshMaterials.erase(submeshKey);
        }
        else
        {
            after.submeshMaterials[submeshKey] = *chosen;
        }

        CommitObjectAcoustics(scene, selection, before, after);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void Editor::DrawAcousticSubmeshField(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<AcousticSettings>& before, const std::string& submeshKey, const std::string& label,
    const ProjectPaths& paths, const ProjectAssetRegistry& registry, bool blocked)
{
    ImGui::PushID(submeshKey.c_str());
    ImGui::TextUnformatted(label.c_str());

    const std::string* reference = nullptr;

    if(before)
    {
        const auto assigned = before->submeshMaterials.find(submeshKey);
        reference = assigned != before->submeshMaterials.end() ? &assigned->second : nullptr;
    }

    const AcousticMaterials::ReferenceDescription description = AcousticMaterials::Describe(
        reference ? std::string_view(*reference) : std::string_view{}, &paths, &registry);
    std::string display = description.display;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = ImGui::GetFrameHeight();
    const bool locatable = reference && AssetID::ParseReference(*reference) && !description.missing;
    const float buttons = buttonWidth * 2.0f + style.ItemSpacing.x * 2.0f + (locatable ? ImGui::CalcTextSize("Locate").x +
        style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f);
    ImGui::SetNextItemWidth((std::max)(1.0f, ImGui::GetContentRegionAvail().x - buttons));

    if(description.missing)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
    }

    ImGui::InputText("##Reference", display.data(), display.size() + 1, ImGuiInputTextFlags_ReadOnly);

    if(description.missing)
    {
        ImGui::PopStyleColor();
    }

    if(ImGui::IsItemHovered() && !description.detail.empty())
    {
        ImGui::SetTooltip("%s", description.detail.c_str());
    }

    if(!blocked && ImGui::BeginDragDropTarget())
    {
        // The Asset Browser's asset payload carries a reference of any type; assignment checks it is acoustic.
        if(const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET_MOVE"))
        {
            if(payload->DataSize == sizeof(AssetBrowser::MovePayload))
            {
                const auto* move = static_cast<const AssetBrowser::MovePayload*>(payload->Data);

                if(move->reference.back() == '\0')
                {
                    AssignAcousticMaterialReference(scene, selection, before, submeshKey, move->reference.data(), registry);
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::Button("...##AcousticPicker", ImVec2(buttonWidth, 0.0f)))
    {
        ImGui::OpenPopup("AcousticMaterialPicker");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!reference);

    if(ImGui::Button("X##AcousticClear", ImVec2(buttonWidth, 0.0f)))
    {
        AcousticSettings after = before.value_or(AcousticSettings{});
        after.submeshMaterials.erase(submeshKey);
        CommitObjectAcoustics(scene, selection, before, after);
    }

    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if(locatable)
    {
        ImGui::SameLine();

        if(ImGui::Button("Locate##AcousticLocate"))
        {
            std::string error;

            if(m_assetBrowser.RevealMaterial(*AssetID::ParseReference(*reference), paths, registry, error))
            {
                m_workspace.AssetBrowserVisible() = true;
            }
            else
            {
                m_acousticEditStatus = "Cannot locate the material: " + error;
            }
        }
    }

    if(!blocked)
    {
        DrawAcousticMaterialPicker(scene, selection, before, submeshKey, registry);
    }

    ImGui::PopID();
}

// Inherent settings of a mesh object: drawn for every object with a mesh, and never offered in Add Component.
void Editor::DrawObjectAcousticControls(IDemoScene& scene, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    std::optional<SceneObjectMetadata> selection(std::in_place);
    SceneObjectDocument object;

    if(!owner->InspectSelectedObject(m_selectedObject, *selection, object) || object.meshReference.empty())
    {
        return;
    }

    const auto& renderObjects = scene.GetScene().GetRenderObjects();
    const RenderObject* renderObject = selection->runtimeIndex < renderObjects.size() ? &renderObjects[selection->runtimeIndex] : nullptr;
    const ImportedMesh* geometry = renderObject && renderObject->mesh ? scene.GetMeshGeometry(*renderObject->mesh) : nullptr;
    const std::optional<AcousticSettings> before = object.acoustics;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection) || IsSceneEditingLocked();

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("Acoustics");

    if(!ImGui::CollapsingHeader("Acoustics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    ImGui::BeginDisabled(blocked);
    bool enabled = before && before->enabled;

    if(ImGui::Checkbox("Enable Acoustics", &enabled))
    {
        // Assignments are kept while disabled, so turning acoustics back on restores them.
        AcousticSettings after = before.value_or(AcousticSettings{});
        after.enabled = enabled;
        CommitObjectAcoustics(scene, *selection, before, after);
    }

    ImGui::EndDisabled();

    if(m_gameState != GameState::Editing)
    {
        ImGui::TextDisabled("Acoustic changes apply the next time Play starts.");
    }

    if(enabled)
    {
        if(!geometry)
        {
            ImGui::TextDisabled("Submeshes are shown once the mesh has loaded.");
        }
        else
        {
            const auto& submeshes = geometry->metadata.submeshes;

            for(size_t index = 0; index < submeshes.size(); ++index)
            {
                const std::string key = AcousticGeometry::SubmeshKey(submeshes[index], index);
                const std::string label = "Submesh " + std::to_string(index) + ": " + submeshes[index].name;
                DrawAcousticSubmeshField(scene, *selection, before, key, label, paths, registry, blocked);
            }

            // Assignments for submeshes this mesh no longer has: reported, and never moved onto another surface.
            if(before)
            {
                for(const auto& [key, reference] : before->submeshMaterials)
                {
                    bool matched = false;

                    for(size_t index = 0; index < submeshes.size() && !matched; ++index)
                    {
                        matched = AcousticGeometry::SubmeshKey(submeshes[index], index) == key;
                    }

                    if(matched)
                    {
                        continue;
                    }

                    ImGui::PushID(key.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
                    ImGui::TextWrapped("Assignment for missing submesh '%s' is ignored.", key.c_str());
                    ImGui::PopStyleColor();
                    ImGui::BeginDisabled(blocked);

                    if(ImGui::SmallButton("Remove"))
                    {
                        AcousticSettings after = *before;
                        after.submeshMaterials.erase(key);
                        CommitObjectAcoustics(scene, *selection, before, after);
                    }

                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
            }
        }

        if(m_audioSystem && m_audioSystem->GetSteamAudioStatus() == Audio::SteamAudioStatus::Active &&
            !m_audioSystem->IsAcousticSimulationAvailable())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, WarningColor);
            ImGui::TextWrapped("Occlusion and transmission are unavailable: Steam Audio's snapshot path failed validation.");
            ImGui::PopStyleColor();
        }
    }

    if(!m_acousticEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_acousticEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
