#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Math/Math.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>

void Editor::DismissControllerManagementPopup()
{
    if(m_controllerManagementPopup)
    {
        CloseComponentPopup(m_controllerManagementPopup->popupID);
    }

    m_controllerManagementPopup.reset();
}

void Editor::ValidateControllerManagementPopup(IDemoScene& scene)
{
    if(!m_controllerManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_controllerManagementPopup) || !owner ||
        !owner->CaptureCharacterControllerInspection(m_controllerManagementPopup->selection.objectID, object) ||
        !object.characterController)
    {
        DismissControllerManagementPopup();
    }
}

bool Editor::CommitObjectCharacterController(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<Physics::CharacterControllerComponent>& before,
    const std::optional<Physics::CharacterControllerComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_controllerEditStatus = "Character Controller authoring requires a stopped scene without pending edits.";
        return false;
    }

    if(after && !Physics::ValidateCharacterControllerComponent(*after, &m_controllerEditStatus))
    {
        return false;
    }

    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_controllerEditStatus = "Character Controller inspection target is stale or blocked.";
        return false;
    }

    if(before == after)
    {
        return owner->SetObjectCharacterController(selection, before, after, &m_controllerEditStatus);
    }

    try
    {
        auto command = std::make_unique<CharacterControllerEdit>(CharacterControllerEdit{
            *m_historySession, selection.objectID, before, after});
        const size_t bytes = sizeof(SceneHistoryEntry) + sizeof(CharacterControllerEdit) +
            command->objectID.size() + command->session.sceneIdentity.size() +
            command->session.projectIdentity.native().size() * sizeof(std::filesystem::path::value_type) +
            command->session.scenePath.native().size() * sizeof(std::filesystem::path::value_type);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_controllerEditStatus = "Character Controller edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectCharacterController(selection, before, after, &m_controllerEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_controllerInspection.reset();
        ++m_controllerControlsRevision;
        m_controllerEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_controllerEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectCharacterControllerControls(IDemoScene& scene)
{
    ValidateControllerManagementPopup(scene);
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    std::optional<SceneObjectMetadata> selection(std::in_place);
    SceneObjectDocument object;

    if(!owner->InspectSelectedObject(m_selectedObject, *selection, object))
    {
        return;
    }

    using Physics::CharacterControllerComponent;
    struct ControllerNumericField
    {
        const char* label;
        float CharacterControllerComponent::* member = nullptr;
        float Vector3::* offsetMember = nullptr;
        bool degrees = false;
    };

    static constexpr std::array<ControllerNumericField, 11> fields{{
        {"Capsule Radius (m)", &CharacterControllerComponent::capsuleRadius},
        {"Cylinder Height (m)", &CharacterControllerComponent::capsuleCylinderHeight},
        {"Local Offset X (m)", nullptr, &Vector3::x},
        {"Local Offset Y (m)", nullptr, &Vector3::y},
        {"Local Offset Z (m)", nullptr, &Vector3::z},
        {"Maximum Horizontal Speed (m/s)", &CharacterControllerComponent::maximumHorizontalSpeed},
        {"Gravity Scale", &CharacterControllerComponent::gravityScale},
        {"Jump Speed (m/s)", &CharacterControllerComponent::jumpSpeed},
        {"Maximum Walkable Slope (deg)", &CharacterControllerComponent::maximumWalkableSlope, nullptr, true},
        {"Ground Snap Distance (m)", &CharacterControllerComponent::groundSnapDistance},
        {"Collision Clearance (m)", &CharacterControllerComponent::collisionClearance}
    }};
    static_assert(fields.size() == std::tuple_size_v<decltype(m_controllerDragDrafts)>);
    const auto fieldValue = [](CharacterControllerComponent& component, const ControllerNumericField& field) -> float&
    {
        if(field.offsetMember)
        {
            return component.localOffset.*field.offsetMember;
        }

        return component.*field.member;
    };
    const auto displayedValue = [&](CharacterControllerComponent component, const ControllerNumericField& field)
    {
        const float value = fieldValue(component, field);
        return field.degrees ? ToDegrees(value) : value;
    };

    const bool changedTarget = !m_controllerInspection || m_controllerInspectionEpoch != m_historyEpoch ||
        m_controllerInspection->objectID != selection->objectID ||
        m_controllerInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_controllerInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    if(changedTarget || m_controllerInspectedValue != object.characterController || blocked)
    {
        ++m_controllerControlsRevision;
        m_controllerInspection = selection;
        m_controllerInspectionEpoch = m_historyEpoch;
        m_controllerInspectedValue = object.characterController;

        if(changedTarget)
        {
            m_controllerEditStatus.clear();
        }

        if(object.characterController)
        {
            for(size_t index = 0; index < fields.size(); ++index)
            {
                m_controllerDragDrafts[index] = displayedValue(*object.characterController, fields[index]);
            }
        }
    }

    if(!object.characterController)
    {
        m_controllerDragDrafts.fill(0.0f);

        if(!m_controllerEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_controllerEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("CharacterController");
    ImGui::Separator();
    ImGui::TextUnformatted("Character Controller");
    ImGui::SameLine();
    ImGui::BeginDisabled(!ResolveComponentEditTarget(scene));

    if(ImGui::SmallButton("..."))
    {
        m_controllerManagementPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("ControllerManagement")};
        ImGui::OpenPopup("ControllerManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("ControllerManagement"))
    {
        ValidateControllerManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_controllerManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateControllerManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            if(m_controllerManagementPopup && current &&
                owner->CaptureCharacterControllerInspection(current->objectID, latest) && latest.characterController &&
                CommitObjectCharacterController(scene, *current, latest.characterController, std::nullopt))
            {
                DismissControllerManagementPopup();
                m_controllerDragDrafts.fill(0.0f);
                removed = true;
            }
        }

        ImGui::PopItemFlag();
        ImGui::EndPopup();
    }

    if(removed)
    {
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return;
    }

    // New numeric IDs prevent a disappearing edit from committing into another component instance.
    ImGui::PushID(static_cast<int>(m_controllerControlsRevision));
    ImGui::BeginDisabled(blocked);

    if(object.characterController)
    {
        ImGui::TextWrapped("Upright local +Y capsule. Cylinder height excludes caps; dimensions ignore visual scale.");

        for(size_t index = 0; index < fields.size(); ++index)
        {
            const auto& field = fields[index];

            float& draft = m_controllerDragDrafts[index];
            // Unbounded drafts: authoritative validation rejects invalid values without typed clamping.
            const InspectorDragSpec drag{field.degrees ? 0.25f : 0.01f};
            const auto result = InspectorDragFloatTransformWidth(field.label, draft, drag);

            if(result == InspectorDragResult::Committed && !blocked)
            {
                if(!std::isfinite(draft))
                {
                    m_controllerEditStatus = "Enter a complete finite number; authored value was preserved.";
                }
                else
                {
                    auto edited = *object.characterController;
                    fieldValue(edited, field) = field.degrees ? ToRadians(draft) : draft;
                    CommitObjectCharacterController(scene, *selection, object.characterController, edited);
                }
            }

            if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
            {
                draft = displayedValue(*object.characterController, field);
            }
        }
    }

    ImGui::EndDisabled();
    // Authored settings above; Play-only runtime diagnostics below, never editable or saved.
    DrawCharacterControllerDiagnostics(*selection, *object.characterController);

    if(!m_controllerEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_controllerEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

bool Editor::ApplyObjectTransformEdits(IDemoScene& scene, std::span<const RenderObjectTransformUpdate> updates)
{
    if(IsPhysicsEditingLocked() || scene.GetScene().HasPhysicsPresentation())
    {
        m_transformEditStatus = "Transform editing requires a stopped scene.";
        return false;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const auto finite = [](const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };

    // Validate every candidate before changing any member of a mixed selection.
    for(const auto& update : updates)
    {
        if(!finite(update.transform.position) || !finite(update.transform.rotation) || !finite(update.transform.scale))
        {
            m_transformEditStatus = "Transform values must be finite; previous transform preserved.";
            return false;
        }

        if(owner && !owner->ValidateObjectTransform(update.id, update.transform, m_transformEditStatus))
        {
            return false;
        }
    }

    if(!scene.GetScene().SetRenderObjectTransforms(updates))
    {
        m_transformEditStatus = "Transform targets changed; previous transforms preserved.";
        return false;
    }

    m_transformEditStatus.clear();
    return true;
}

bool Editor::ApplyObjectTransformEdit(IDemoScene& scene, RenderObjectID index, const Transform& transform)
{
    const RenderObjectTransformUpdate update{index, transform};
    return ApplyObjectTransformEdits(scene, {&update, 1});
}
