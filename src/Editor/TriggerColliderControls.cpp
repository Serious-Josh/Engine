#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Math/Math.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

std::optional<SceneObjectMetadata> Editor::ResolveComponentEditTarget(IDemoScene& scene)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || owner->IsArchiveActivation() || !m_workspace.InspectorVisible() ||
        m_inspectorTarget != InspectorTarget::SceneObject || !m_objectInspectionIdentity ||
        IsPhysicsEditingLocked() || HasPendingSceneHistoryOperation() || m_sceneOperationPending ||
        HasBlockingObjectCreation() || m_sceneObjectDeletionPending || m_sceneObjectDeletionRequest ||
        m_meshCreationRequest || m_primitiveCreationRequest || m_emptyObjectCreationRequest || m_fileRequest ||
        m_materialAssignmentRequest || !m_historySession || !RenameSessionMatches(scene, *m_historySession))
    {
        return std::nullopt;
    }

    const auto selection = owner->GetObjectMetadata(m_selectedObject);
    const auto& identity = *m_objectInspectionIdentity;

    if(!selection || identity.scene != &scene.GetScene() ||
        identity.revision != selection->structureRevision || identity.runtimeIdentity != selection->runtimeIdentity ||
        identity.sceneID != selection->sceneID || identity.objectID != selection->objectID)
    {
        return std::nullopt;
    }

    return selection;
}

void Editor::CloseComponentPopup(ImGuiID popupID)
{
    if(!ImGui::GetCurrentContext())
    {
        return;
    }

    const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;

    for(int index = 0; index < popups.Size; ++index)
    {
        if(popups[index].PopupId == popupID)
        {
            ImGui::ClosePopupToLevel(index, true);
            break;
        }
    }
}

ImVec2 Editor::CalculateAddComponentPopupPosition(ImVec2 buttonMinimum, ImVec2 buttonMaximum,
    float contentCenterX, ImVec2 popupSize, ImVec2 workMinimum, ImVec2 workMaximum, float gap)
{
    const float belowY = buttonMaximum.y + gap;
    const float aboveY = buttonMinimum.y - gap - popupSize.y;
    const bool fitsBelow = belowY + popupSize.y <= workMaximum.y;
    const bool fitsAbove = aboveY >= workMinimum.y;
    float y = belowY;

    if(!fitsBelow && fitsAbove)
    {
        y = aboveY;
    }
    else if(!fitsBelow)
    {
        // Neither side fits: use the roomier side, kept inside the work area.
        const float roomBelow = workMaximum.y - belowY;
        const float roomAbove = buttonMinimum.y - gap - workMinimum.y;
        y = roomBelow >= roomAbove ? belowY : (std::max)(workMinimum.y, aboveY);
    }

    // Center on the content, then shift horizontally to stay within the work area.
    float x = contentCenterX - popupSize.x * 0.5f;
    x = (std::min)(x, workMaximum.x - popupSize.x);
    x = (std::max)(x, workMinimum.x);
    return {x, y};
}

void Editor::DismissAddComponentPopup()
{
    if(m_addComponentPopup)
    {
        CloseComponentPopup(m_addComponentPopup->popupID);
    }

    m_addComponentPopup.reset();
    m_componentSearch.fill(0);
    m_focusComponentSearch = false;
}

void Editor::DismissTriggerManagementPopup()
{
    if(m_triggerManagementPopup)
    {
        CloseComponentPopup(m_triggerManagementPopup->popupID);
    }

    m_triggerManagementPopup.reset();
}

bool Editor::ComponentPopupMatches(IDemoScene& scene, const ComponentPopupTarget& target)
{
    const auto current = ResolveComponentEditTarget(scene);
    return current && target.inspectionEpoch == m_historyEpoch && RenameSessionMatches(scene, target.session) &&
        current->objectID == target.selection.objectID && current->sceneID == target.selection.sceneID &&
        current->runtimeIdentity == target.selection.runtimeIdentity &&
        current->structureRevision == target.selection.structureRevision &&
        current->runtimeIndex == target.selection.runtimeIndex && !ImGui::IsKeyPressed(ImGuiKey_Escape) &&
        ImGui::IsPopupOpen(target.popupID, ImGuiPopupFlags_AnyPopupLevel);
}

void Editor::ValidateAddComponentPopup(IDemoScene& scene)
{
    if(m_addComponentPopup && !ComponentPopupMatches(scene, *m_addComponentPopup))
    {
        DismissAddComponentPopup();
    }
}

void Editor::ValidateTriggerManagementPopup(IDemoScene& scene)
{
    if(!m_triggerManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_triggerManagementPopup) || !owner ||
        !owner->CaptureTriggerColliderInspection(m_triggerManagementPopup->selection.objectID, object) ||
        !object.triggerCollider)
    {
        DismissTriggerManagementPopup();
    }
}

void Editor::DrawAddComponentControls(IDemoScene& scene)
{
    ValidateAddComponentPopup(scene);
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner || owner->IsArchiveActivation())
    {
        return;
    }

    const auto selection = owner->GetObjectMetadata(m_selectedObject);
    SceneObjectDocument object;

    if(!selection || !owner->CaptureTriggerColliderInspection(selection->objectID, object))
    {
        DismissAddComponentPopup();
        return;
    }

    const bool blocked = !ResolveComponentEditTarget(scene);
    ImGui::PushID("GenericOptionals");
    ImGui::Spacing();

    const float availableWidth = (std::max)(0.0f, ImGui::GetContentRegionAvail().x);

    if(availableWidth <= 0.0f)
    {
        ImGui::PopID();
        return;
    }

    const float naturalWidth = ImGui::CalcTextSize("Add Component").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float buttonWidth = (std::min)(naturalWidth, availableWidth);
    const float contentStart = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(contentStart + (availableWidth - buttonWidth) * 0.5f);
    ImGui::BeginDisabled(blocked);

    if(ImGui::Button("Add Component", ImVec2(buttonWidth, 0.0f)))
    {
        m_componentSearch.fill(0);
        m_focusComponentSearch = true;
        m_addComponentPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("AddComponentPopup")};
        ImGui::OpenPopup("AddComponentPopup");
    }

    // Capture this frame's button rectangle: it already reflects Inspector scrolling.
    const ImVec2 buttonMinimum = ImGui::GetItemRectMin();
    const ImVec2 buttonMaximum = ImGui::GetItemRectMax();
    ImGui::EndDisabled();

    // Without an explicit position ImGui anchors the popup to the mouse position at opening and
    // flips it upward over unrelated sections when the button sits low in a long Inspector.
    if(m_addComponentPopup)
    {
        ImVec2 popupSize{};
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;

        for(int index = 0; index < popups.Size; ++index)
        {
            // Previous-frame size; an auto-fitting popup is hidden on its first frame.
            if(popups[index].PopupId == m_addComponentPopup->popupID && popups[index].Window)
            {
                popupSize = popups[index].Window->Size;
            }
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 workMaximum{viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y};
        // The button is centered on the content area, so its center is the content center.
        const float contentCenterX = (buttonMinimum.x + buttonMaximum.x) * 0.5f;
        ImGui::SetNextWindowPos(CalculateAddComponentPopupPosition(buttonMinimum, buttonMaximum, contentCenterX,
            popupSize, viewport->WorkPos, workMaximum, ImGui::GetStyle().ItemInnerSpacing.y));
    }

    if(ImGui::BeginPopup("AddComponentPopup"))
    {
        ValidateAddComponentPopup(scene);

        if(m_focusComponentSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusComponentSearch = false;
        }

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        const bool searchEnter = ImGui::InputTextWithHint("##ComponentSearch", "Search components...",
            m_componentSearch.data(), m_componentSearch.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string query(m_componentSearch.data());
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        const bool triggerAvailable = !object.triggerCollider;
        const bool controllerAbsent = !object.characterController;
        std::string controllerReason;

        if(object.physics)
        {
            controllerReason = "Remove Physics before adding Character Controller.";
        }
        else
        {
            Physics::ValidateCharacterControllerTransform(Physics::CharacterControllerComponent{},
                object.transform, &controllerReason);
        }

        const bool controllerAvailable = controllerAbsent && controllerReason.empty();
        const bool triggerMatches = triggerAvailable && std::string_view("trigger collider").find(query) != std::string_view::npos;
        const bool controllerMatches = controllerAbsent && std::string_view("character controller").find(query) != std::string_view::npos;
        const int eligibleMatches = static_cast<int>(triggerMatches) + static_cast<int>(controllerMatches && controllerAvailable);
        ImGui::Separator();
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        const bool triggerSelected = triggerMatches && ImGui::MenuItem("Trigger Collider");
        bool controllerSelected = false;

        if(controllerMatches)
        {
            controllerSelected = ImGui::MenuItem("Character Controller", nullptr, false, controllerAvailable);

            if(!controllerAvailable)
            {
                ImGui::TextWrapped("%s", controllerReason.c_str());
            }
        }

        if(!triggerAvailable && !controllerAvailable)
        {
            ImGui::TextDisabled("No components available.");
        }
        else if(eligibleMatches == 0)
        {
            ImGui::TextDisabled("No matching components.");
        }

        const bool addTrigger = triggerMatches && (triggerSelected || (searchEnter && eligibleMatches == 1));
        const bool addController = controllerAvailable && controllerMatches &&
            (controllerSelected || (searchEnter && eligibleMatches == 1));

        // Enter deactivates the search field; keep it focused when the query selects nothing.
        if(searchEnter && !addTrigger && !addController)
        {
            m_focusComponentSearch = true;
        }

        if(m_addComponentPopup && (addTrigger || addController))
        {
            ValidateAddComponentPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            if(m_addComponentPopup && current && owner->CaptureTriggerColliderInspection(current->objectID, latest))
            {
                bool committed = false;

                if(addTrigger && !latest.triggerCollider)
                {
                    committed = CommitObjectTriggerCollider(scene, *current, std::nullopt, Physics::TriggerColliderComponent{});
                }
                else if(addController && !latest.characterController && !latest.physics)
                {
                    committed = CommitObjectCharacterController(scene, *current, std::nullopt, Physics::CharacterControllerComponent{});
                }

                if(committed)
                {
                    DismissAddComponentPopup();
                }
            }
        }

        ImGui::PopItemFlag();
        ImGui::EndPopup();
    }

    ImGui::PopID();
}

bool Editor::CommitObjectTriggerCollider(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<Physics::TriggerColliderComponent>& before,
    const std::optional<Physics::TriggerColliderComponent>& after)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_triggerEditStatus = "Trigger Collider authoring requires a stopped scene without pending edits.";
        return false;
    }

    if(after && !Physics::ValidateTriggerColliderComponent(*after, &m_triggerEditStatus))
    {
        return false;
    }

    if(before == after)
    {
        return true;
    }

    try
    {
        auto command = std::make_unique<TriggerColliderEdit>(TriggerColliderEdit{
            *m_historySession, selection.objectID, before, after});
        const size_t bytes = sizeof(SceneHistoryEntry) + sizeof(TriggerColliderEdit) +
            command->objectID.size() + command->session.sceneIdentity.size() +
            command->session.projectIdentity.native().size() * sizeof(std::filesystem::path::value_type) +
            command->session.scenePath.native().size() * sizeof(std::filesystem::path::value_type);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_triggerEditStatus = "Trigger Collider edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectTriggerCollider(selection, before, after, &m_triggerEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_triggerInspection.reset();
        ++m_triggerControlsRevision;
        m_triggerEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_triggerEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectTriggerColliderControls(IDemoScene& scene)
{
    ValidateTriggerManagementPopup(scene);
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(!owner)
    {
        return;
    }

    const auto selection = owner->GetObjectMetadata(m_selectedObject);
    SceneObjectDocument object;

    if(!selection || !owner->CaptureTriggerColliderInspection(selection->objectID, object))
    {
        return;
    }

    using Physics::TriggerColliderComponent;
    using Physics::ColliderType;
    using Physics::BoxDimensions;

    struct TriggerNumericField
    {
        const char* label;
        std::optional<ColliderType> shape;
        InspectorDragSpec drag;
        float TriggerColliderComponent::* componentMember = nullptr;
        float BoxDimensions::* boxMember = nullptr;
        float Vector3::* offsetMember = nullptr;
        float Vector3::* rotationMember = nullptr;
    };

    constexpr float largestFloat = (std::numeric_limits<float>::max)();
    static constexpr InspectorDragSpec lengthDrag{0.01f, 0.0f, largestFloat};
    static constexpr InspectorDragSpec offsetDrag{0.01f};
    static constexpr InspectorDragSpec angleDrag{0.25f};
    static constexpr std::array<TriggerNumericField, 12> fields{{
        {"Width (X, m)", ColliderType::Box, lengthDrag, nullptr, &BoxDimensions::width},
        {"Height (Y, m)", ColliderType::Box, lengthDrag, nullptr, &BoxDimensions::height},
        {"Depth (Z, m)", ColliderType::Box, lengthDrag, nullptr, &BoxDimensions::depth},
        {"Radius (m)##Sphere", ColliderType::Sphere, lengthDrag, &TriggerColliderComponent::sphereRadius},
        {"Radius (m)##Capsule", ColliderType::Capsule, lengthDrag, &TriggerColliderComponent::capsuleRadius},
        {"Cylinder Height (Y, m)", ColliderType::Capsule, lengthDrag, &TriggerColliderComponent::capsuleCylinderHeight},
        {"Local Offset X (m)", std::nullopt, offsetDrag, nullptr, nullptr, &Vector3::x},
        {"Local Offset Y (m)", std::nullopt, offsetDrag, nullptr, nullptr, &Vector3::y},
        {"Local Offset Z (m)", std::nullopt, offsetDrag, nullptr, nullptr, &Vector3::z},
        {"Local Rotation X (deg)", ColliderType::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::x},
        {"Local Rotation Y (deg)", ColliderType::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::y},
        {"Local Rotation Z (deg)", ColliderType::Capsule, angleDrag, nullptr, nullptr, nullptr, &Vector3::z}
    }};
    static_assert(fields.size() == std::tuple_size_v<decltype(m_triggerDragDrafts)>);

    const auto fieldValue = [](TriggerColliderComponent& component, const TriggerNumericField& field) -> float&
    {
        if(field.boxMember)
        {
            return component.boxDimensions.*field.boxMember;
        }

        if(field.offsetMember)
        {
            return component.colliderLocalPositionOffset.*field.offsetMember;
        }

        if(field.rotationMember)
        {
            return component.capsuleLocalRotation.*field.rotationMember;
        }

        return component.*field.componentMember;
    };
    const auto displayedValue = [&](TriggerColliderComponent component, const TriggerNumericField& field)
    {
        const float value = fieldValue(component, field);
        return field.rotationMember ? ToDegrees(value) : value;
    };

    const bool changedTarget = !m_triggerInspection || m_triggerInspectionEpoch != m_historyEpoch ||
        m_triggerInspection->objectID != selection->objectID ||
        m_triggerInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_triggerInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene);

    if(changedTarget || m_triggerInspectedValue != object.triggerCollider || blocked)
    {
        ++m_triggerControlsRevision;
        m_triggerInspection = selection;
        m_triggerInspectionEpoch = m_historyEpoch;
        m_triggerInspectedValue = object.triggerCollider;

        if(changedTarget)
        {
            m_triggerEditStatus.clear();
        }

        if(object.triggerCollider)
        {
            for(size_t index = 0; index < fields.size(); ++index)
            {
                m_triggerDragDrafts[index] = displayedValue(*object.triggerCollider, fields[index]);
            }
        }
    }

    if(!object.triggerCollider)
    {
        m_triggerDragDrafts.fill(0.0f);

        if(!m_triggerEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_triggerEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("TriggerCollider");
    ImGui::Separator();
    ImGui::TextUnformatted("Trigger Collider");
    ImGui::SameLine();
    ImGui::BeginDisabled(!ResolveComponentEditTarget(scene));

    if(ImGui::SmallButton("..."))
    {
        m_triggerManagementPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("TriggerManagement")};
        ImGui::OpenPopup("TriggerManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("TriggerManagement"))
    {
        ValidateTriggerManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_triggerManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateTriggerManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            if(m_triggerManagementPopup && current &&
                owner->CaptureTriggerColliderInspection(current->objectID, latest) && latest.triggerCollider &&
                CommitObjectTriggerCollider(scene, *current, latest.triggerCollider, std::nullopt))
            {
                DismissTriggerManagementPopup();
                m_triggerDragDrafts.fill(0.0f);
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
    ImGui::PushID(static_cast<int>(m_triggerControlsRevision));
    ImGui::BeginDisabled(blocked);

    if(object.triggerCollider)
    {
        int shape = static_cast<int>(object.triggerCollider->colliderType);

        if(ImGui::Combo("Trigger Shape", &shape, "Box\0Sphere\0Capsule\0"))
        {
            auto edited = *object.triggerCollider;
            edited.colliderType = static_cast<ColliderType>(shape);
            CommitObjectTriggerCollider(scene, *selection, object.triggerCollider, edited);
        }

        ImGui::TextWrapped("Full dimensions and local offset in meters; independent of visual scale.");

        if(object.triggerCollider->colliderType == ColliderType::Capsule)
        {
            ImGui::TextWrapped("Capsule axis is local +Y. Cylinder height excludes the rounded ends.");
        }

        for(size_t index = 0; index < fields.size(); ++index)
        {
            const auto& field = fields[index];

            if(field.shape && *field.shape != object.triggerCollider->colliderType)
            {
                continue;
            }

            float& draft = m_triggerDragDrafts[index];
            const auto result = InspectorDragFloatTransformWidth(field.label, draft, field.drag);

            if(result == InspectorDragResult::Committed && !blocked)
            {
                if(!std::isfinite(draft))
                {
                    m_triggerEditStatus = "Enter a complete finite number; authored value was preserved.";
                }
                else
                {
                    auto edited = *object.triggerCollider;
                    fieldValue(edited, field) = field.rotationMember ? ToRadians(draft) : draft;
                    CommitObjectTriggerCollider(scene, *selection, object.triggerCollider, edited);
                }
            }

            if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
            {
                draft = displayedValue(*object.triggerCollider, field);
            }
        }
    }

    ImGui::EndDisabled();

    if(!m_triggerEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_triggerEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
