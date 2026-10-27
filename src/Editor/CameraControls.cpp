#include "Editor/Editor.h"
#include "Editor/InspectorNumericDrag.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Math/Math.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

void Editor::DismissCameraManagementPopup()
{
    if(m_cameraManagementPopup)
    {
        CloseComponentPopup(m_cameraManagementPopup->popupID);
    }

    m_cameraManagementPopup.reset();
}

void Editor::ValidateCameraManagementPopup(IDemoScene& scene)
{
    if(!m_cameraManagementPopup)
    {
        return;
    }

    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    SceneObjectDocument object;

    if(!ComponentPopupMatches(scene, *m_cameraManagementPopup) || !owner ||
        !owner->CaptureCameraInspection(m_cameraManagementPopup->selection.objectID, object) || !object.camera)
    {
        DismissCameraManagementPopup();
    }
}

size_t Editor::CameraEditPayloadBytes(const CameraEdit& edit)
{
    const auto pathBytes = [](const std::filesystem::path& path)
    {
        return path.native().capacity() * sizeof(std::filesystem::path::value_type);
    };
    // Camera values are stored inline in each change; only strings and the vector allocate.
    size_t bytes = sizeof(SceneHistoryEntry) + sizeof(CameraEdit) + edit.changes.capacity() * sizeof(CameraChange) +
        edit.session.sceneIdentity.capacity() + pathBytes(edit.session.projectIdentity) +
        pathBytes(edit.session.scenePath);

    for(const auto& change : edit.changes)
    {
        bytes += change.objectID.capacity();
    }

    return bytes;
}

bool Editor::CommitObjectCameras(IDemoScene& scene, std::span<const SceneCameraChange> changes)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(IsPhysicsEditingLocked() || !owner || !m_historySession ||
        !RenameSessionMatches(scene, *m_historySession) || HasPendingSceneHistoryOperation() ||
        m_sceneOperationPending || HasBlockingObjectCreation())
    {
        m_cameraEditStatus = "Camera authoring requires a stopped scene without pending edits.";
        return false;
    }

    for(const auto& change : changes)
    {
        if(change.replacement && !ValidateCameraComponent(*change.replacement, &m_cameraEditStatus))
        {
            m_cameraEditStatus += " The authored value was preserved.";
            return false;
        }
    }

    const bool changed = std::any_of(changes.begin(), changes.end(),
        [](const SceneCameraChange& change) { return change.expected != change.replacement; });

    if(!changed)
    {
        // Still checks staleness, but a no-op never records history or dirties the scene.
        return owner->SetObjectCameras(changes, &m_cameraEditStatus);
    }

    try
    {
        auto command = std::make_unique<CameraEdit>(CameraEdit{*m_historySession, {}});
        command->changes.reserve(changes.size());

        for(const auto& change : changes)
        {
            command->changes.push_back({change.selection.objectID, change.expected, change.replacement});
        }

        const size_t bytes = CameraEditPayloadBytes(*command);

        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_cameraEditStatus = "Camera edit exceeds the scene history payload limit.";
            return false;
        }

        SceneHistoryEntry entry{std::move(command), m_historyEpoch, bytes};
        // Reserve before mutation; appending the accepted command is noexcept.
        m_sceneHistory.reserve(kSceneHistoryCapacity);

        if(!owner->SetObjectCameras(changes, &m_cameraEditStatus))
        {
            return false;
        }

        AppendSceneHistoryEntry(std::move(entry));
        m_cameraInspection.reset();
        ++m_cameraControlsRevision;
        m_cameraEditStatus.clear();
        return true;
    }
    catch(const std::exception& exception)
    {
        m_cameraEditStatus = exception.what();
        return false;
    }
}

bool Editor::CommitObjectCamera(IDemoScene& scene, const SceneObjectMetadata& selection,
    const std::optional<CameraComponent>& before, const std::optional<CameraComponent>& after)
{
    const auto current = ResolveComponentEditTarget(scene);

    if(!current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_cameraEditStatus = "Camera inspection target is stale or blocked.";
        return false;
    }

    const SceneCameraChange change{selection, before, after};
    return CommitObjectCameras(scene, {&change, 1});
}

bool Editor::CommitCameraMainDesignation(IDemoScene& scene, const SceneObjectMetadata& selection, bool main)
{
    auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
    const auto current = ResolveComponentEditTarget(scene);

    if(!owner || !current || current->objectID != selection.objectID || current->sceneID != selection.sceneID ||
        current->runtimeIdentity != selection.runtimeIdentity || current->structureRevision != selection.structureRevision ||
        current->runtimeIndex != selection.runtimeIndex)
    {
        m_cameraEditStatus = "Camera inspection target is stale or blocked.";
        return false;
    }

    try
    {
        // Resolve every affected Camera by stable ID from the authored document this frame;
        // never by array position, name or renderer index.
        std::vector<SceneCameraChange> changes;
        const auto& objects = owner->m_runtime->GetFileScene().GetDocument().objects;

        for(const auto& object : objects)
        {
            if(!object.camera)
            {
                continue;
            }

            const bool target = object.id == selection.objectID;

            if(target && object.camera->main == main)
            {
                return true;
            }

            if(!target && !(main && object.camera->main))
            {
                continue;
            }

            const auto metadata = target ? current : owner->GetObjectMetadata(object.id, &m_cameraEditStatus);

            if(!metadata)
            {
                return false;
            }

            auto after = *object.camera;
            after.main = target && main;
            changes.push_back({*metadata, object.camera, after});
        }

        const bool targetFound = std::any_of(changes.begin(), changes.end(),
            [&](const SceneCameraChange& change) { return change.selection.objectID == selection.objectID; });

        if(!targetFound)
        {
            m_cameraEditStatus = "Main Camera designation requires an object with a Camera.";
            return false;
        }

        return CommitObjectCameras(scene, changes);
    }
    catch(const std::exception& exception)
    {
        m_cameraEditStatus = exception.what();
        return false;
    }
}

void Editor::DrawObjectCameraControls(IDemoScene& scene)
{
    ValidateCameraManagementPopup(scene);
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

    struct CameraNumericField
    {
        const char* label;
        float CameraComponent::* member;
        bool degrees = false;
        float speed = 0.01f;
    };

    // Drag speeds only; CAM.2's ValidateCameraComponent is the sole range contract.
    static constexpr std::array<CameraNumericField, 3> fields{{
        {"Vertical Field of View (deg)", &CameraComponent::verticalFov, true, 0.25f},
        {"Near Clip Plane (m)", &CameraComponent::nearPlane, false, 0.01f},
        {"Far Clip Plane (m)", &CameraComponent::farPlane, false, 1.0f}
    }};
    static_assert(fields.size() == std::tuple_size_v<decltype(m_cameraDragDrafts)>);
    const auto displayedValue = [](const CameraComponent& camera, const CameraNumericField& field)
    {
        const float value = camera.*field.member;
        return field.degrees ? ToDegrees(value) : value;
    };

    const bool changedTarget = !m_cameraInspection || m_cameraInspectionEpoch != m_historyEpoch ||
        m_cameraInspection->objectID != selection->objectID ||
        m_cameraInspection->runtimeIdentity != selection->runtimeIdentity ||
        m_cameraInspection->structureRevision != selection->structureRevision;
    const bool blocked = !ResolveComponentEditTarget(scene, *selection);

    // Any owner, session, history or value change discards drafts and retires widget IDs.
    if(changedTarget || m_cameraInspectedValue != object.camera || blocked)
    {
        ++m_cameraControlsRevision;
        m_cameraInspection = selection;
        m_cameraInspectionEpoch = m_historyEpoch;
        m_cameraInspectedValue = object.camera;

        if(changedTarget)
        {
            m_cameraEditStatus.clear();
        }

        if(object.camera)
        {
            for(size_t index = 0; index < fields.size(); ++index)
            {
                m_cameraDragDrafts[index] = displayedValue(*object.camera, fields[index]);
            }
        }
    }

    if(!object.camera)
    {
        m_cameraDragDrafts.fill(0.0f);

        if(!m_cameraEditStatus.empty())
        {
            ImGui::TextWrapped("%s", m_cameraEditStatus.c_str());
        }

        return;
    }

    ImGui::PushID(static_cast<int>(m_historyEpoch));
    ImGui::PushID(selection->objectID.c_str());
    ImGui::PushID("Camera");
    ImGui::Separator();
    ImGui::TextUnformatted("Camera");
    ImGui::SameLine();
    ImGui::BeginDisabled(blocked);

    if(ImGui::SmallButton("..."))
    {
        m_cameraManagementPopup = ComponentPopupTarget{
            *m_historySession, *selection, m_historyEpoch, ImGui::GetID("CameraManagement")};
        ImGui::OpenPopup("CameraManagement");
    }

    ImGui::EndDisabled();
    bool removed = false;

    if(ImGui::BeginPopup("CameraManagement"))
    {
        ValidateCameraManagementPopup(scene);
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        if(m_cameraManagementPopup && ImGui::MenuItem("Remove Component"))
        {
            ValidateCameraManagementPopup(scene);
            const auto current = ResolveComponentEditTarget(scene);
            SceneObjectDocument latest;

            // Removing the Main Camera leaves the scene without Main; nothing is promoted.
            if(m_cameraManagementPopup && current && owner->CaptureCameraInspection(current->objectID, latest) &&
                latest.camera && CommitObjectCamera(scene, *current, latest.camera, std::nullopt))
            {
                DismissCameraManagementPopup();
                m_cameraDragDrafts.fill(0.0f);
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
    ImGui::PushID(static_cast<int>(m_cameraControlsRevision));
    ImGui::BeginDisabled(blocked);
    const CameraComponent camera = *object.camera;
    bool enabled = camera.enabled;

    if(ImGui::Checkbox("Enabled", &enabled) && !blocked)
    {
        auto edited = camera;
        edited.enabled = enabled;
        CommitObjectCamera(scene, *selection, object.camera, edited);
    }

    ImGui::TextUnformatted("Projection: Perspective");

    for(size_t index = 0; index < fields.size(); ++index)
    {
        const auto& field = fields[index];
        float& draft = m_cameraDragDrafts[index];
        // Unbounded drafts: authoritative validation rejects invalid values without clamping.
        const InspectorDragSpec drag{field.speed};
        const auto result = InspectorDragFloatTransformWidth(field.label, draft, drag);

        if(result == InspectorDragResult::Committed && !blocked)
        {
            if(!std::isfinite(draft))
            {
                m_cameraEditStatus = "Enter a complete finite number; authored value was preserved.";
            }
            else
            {
                auto edited = camera;
                edited.*field.member = field.degrees ? ToRadians(draft) : draft;
                CommitObjectCamera(scene, *selection, object.camera, edited);
            }
        }

        if(result == InspectorDragResult::Committed || result == InspectorDragResult::Cancelled)
        {
            draft = displayedValue(camera, field);
        }
    }

    bool main = camera.main;

    // Checking transfers Main from any other Camera in one undoable edit; unchecking leaves none.
    if(ImGui::Checkbox("Main Camera", &main) && !blocked)
    {
        CommitCameraMainDesignation(scene, *selection, main);
    }

    ImGui::EndDisabled();

    if(!m_cameraEditStatus.empty())
    {
        ImGui::TextWrapped("%s", m_cameraEditStatus.c_str());
    }

    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}
