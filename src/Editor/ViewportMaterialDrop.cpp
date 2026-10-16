#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"

#include <algorithm>
#include <utility>

size_t Editor::MaterialBindingPayloadBytes(const MaterialBindingsEdit& edit)
{
    size_t bytes = sizeof(SceneHistoryEntry) + sizeof(MaterialBindingsEdit);
    const auto text = [&bytes](const std::string& value) { bytes += value.capacity() + 1; };
    text(edit.objectID);
    text(edit.meshReference);
    text(edit.fallbackReference);
    text(edit.session.sceneIdentity);
    bytes += (edit.session.projectIdentity.native().capacity() + edit.session.scenePath.native().capacity() + 2) *
        sizeof(std::filesystem::path::value_type);
    for(const auto* bindings : {&edit.before, &edit.after})
        for(const auto& [slot, reference] : *bindings)
        {
            bytes += sizeof(std::pair<const std::string, std::string>) + 4 * sizeof(void*);
            text(slot);
            text(reference);
        }
    bytes += edit.layout.materialSlots.capacity() * sizeof(MeshMaterialSlot) +
        edit.layout.submeshes.capacity() * sizeof(MeshSubmesh);
    for(const auto& slot : edit.layout.materialSlots) { text(slot.id); text(slot.name); }
    for(const auto& range : edit.layout.submeshes) text(range.name);
    return bytes;
}

void Editor::QueueViewportMaterialDrop(const IDemoScene& scene,
    const SceneObjectMetadata& target, const AssetID& material)
{
    if (IsPhysicsEditingLocked())
    {
        return;
    }

    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    if(!owner || !m_historySession || !RenameSessionMatches(scene, *m_historySession) ||
        (HasPendingSceneHistoryOperation() && !m_pendingMaterialBinding) || HasBlockingObjectCreation() || m_fileRequest ||
        m_materialAssignmentRequest || m_sceneOperationPending || !SceneObjectDeletionMatches(scene, target))
    {
        m_materialDropStatus = "Material drop rejected: scene or editor operation changed.";
        return;
    }
    try
    {
        const auto current = owner->GetObjectMetadata(target.objectID);
        const auto* mesh = current ? scene.GetScene().GetRenderObjects()[current->runtimeIndex].mesh : nullptr;
        std::string error;
        if(!current || !mesh || current->bindingRevision != target.bindingRevision ||
            !ValidateMeshMetadata(mesh->GetMetadata(), mesh->IsIndexed() ? mesh->GetIndexCount() : mesh->GetVertexCount(), error))
        {
            m_materialDropStatus = "Material drop rejected: resident mesh or slot layout is unavailable.";
            return;
        }
        auto edit = std::make_unique<MaterialBindingsEdit>();
        edit->session = *m_historySession;
        edit->objectID = current->objectID;
        edit->meshReference = current->meshReference;
        edit->fallbackReference = current->materialReference;
        edit->layout = mesh->GetMetadata();
        edit->before = current->materialSlotBindings;
        for(const auto& slot : edit->layout.materialSlots)
            edit->after.emplace(slot.id, material.ToReference());
        if(edit->before == edit->after && !m_pendingMaterialBinding)
        {
            m_materialDropStatus = "Material is already assigned to every slot.";
            return;
        }
        const auto bytes = MaterialBindingPayloadBytes(*edit);
        if(bytes > kSceneHistoryPayloadBytes)
        {
            m_materialDropStatus = "Material drop rejected: command exceeds the scene history limit.";
            return;
        }
        m_sceneHistory.reserve(kSceneHistoryCapacity);
        const auto operation = AllocateSceneHistoryOperationID();
        MaterialBindingRequest request{*current, material, edit->after, edit->layout, operation, m_historyEpoch};
        PendingMaterialBinding pending{operation, m_historyEpoch, m_historyCursor, std::nullopt,
            SceneHistoryEntry{std::move(edit), m_historyEpoch, bytes}};
        // History storage and both outbound/retained copies are ready before the
        // Engine can commit. Acceptance only moves the staged command into history.
        m_pendingMaterialBinding = std::move(pending);
        m_viewportMaterialRequest = std::move(request);
        m_materialDropStatus = "Material assignment queued...";
    }
    catch(const std::exception& exception)
    {
        m_materialDropStatus = "Material drop preparation failed: " + std::string(exception.what());
    }
}

void Editor::QueueMaterialBindingReplay(IDemoScene& scene, const MaterialBindingsEdit& edit, bool redo)
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner ? owner->GetObjectMetadata(edit.objectID) : std::nullopt;
    const auto* mesh = current ? scene.GetScene().GetRenderObjects()[current->runtimeIndex].mesh : nullptr;
    if(!owner || !RenameSessionMatches(scene, edit.session) || !current || !mesh ||
        current->meshReference != edit.meshReference || current->materialReference != edit.fallbackReference ||
        mesh->GetMetadata() != edit.layout || current->materialSlotBindings != (redo ? edit.before : edit.after))
    {
        ReportSceneHistoryFailure("Material bindings, mesh layout or scene changed; cursor preserved.");
        return;
    }
    try
    {
        const auto operation = AllocateSceneHistoryOperationID();
        MaterialBindingRequest request{*current, std::nullopt, redo ? edit.after : edit.before, edit.layout, operation, m_historyEpoch};
        PendingMaterialBinding pending{operation, m_historyEpoch, m_historyCursor, redo, std::nullopt};
        m_pendingMaterialBinding = std::move(pending);
        m_viewportMaterialRequest = std::move(request);
        m_materialDropStatus = redo ? "Material redo queued..." : "Material undo queued...";
    }
    catch(const std::exception& exception) { ReportSceneHistoryFailure(exception.what()); }
}

std::optional<Editor::MaterialBindingRequest> Editor::TakeViewportMaterialRequest()
{
    return std::exchange(m_viewportMaterialRequest, std::nullopt);
}

bool Editor::IsViewportMaterialRequestCurrent(const MaterialBindingRequest& request) const noexcept
{
    return m_pendingMaterialBinding && m_historySession && request.epoch == m_historyEpoch &&
        request.epoch == m_historySession->epoch && m_pendingMaterialBinding->epoch == request.epoch &&
        m_pendingMaterialBinding->operationID == request.operationID &&
        m_pendingMaterialBinding->cursor == m_historyCursor;
}

void Editor::CompleteViewportMaterialRequest(const MaterialBindingRequest& request,
    bool succeeded, bool published, std::string_view message) noexcept
{
    if(!IsViewportMaterialRequestCurrent(request)) return;
    if(succeeded && published)
    {
        if(m_pendingMaterialBinding->forward)
            AppendSceneHistoryEntry(std::move(*m_pendingMaterialBinding->forward));
        else if(m_pendingMaterialBinding->redo)
            m_historyCursor = *m_pendingMaterialBinding->redo ? m_historyCursor + 1 : m_historyCursor - 1;
    }
    m_pendingMaterialBinding.reset();
    m_viewportMaterialRequest.reset();
    try
    {
        m_materialDropStatus = succeeded ? (published ? "Material assigned. Save the scene to persist it." :
            "Material bindings are unchanged.") : "Material assignment rejected: " + std::string(message);
        if(!succeeded) ReportSceneHistoryFailure(std::string(message));
    }
    catch(...) {} // History commit/acknowledgment above must survive diagnostic allocation failure.
}

void Editor::NotifyViewportMaterialRetry(const MaterialBindingRequest& request, std::string_view message)
{
    if(IsViewportMaterialRequestCurrent(request))
        m_materialDropStatus = "Waiting for material publication: " + std::string(message);
}

std::optional<size_t> Editor::GetMaterialDropHighlight(const IDemoScene& scene) const
{
    const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene);
    const auto current = owner && m_materialDropHover ? owner->GetObjectMetadata(m_materialDropHover->objectID) : std::nullopt;
    if(current && current->sceneID == m_materialDropHover->sceneID &&
        current->runtimeIdentity == m_materialDropHover->runtimeIdentity &&
        current->structureRevision == m_materialDropHover->structureRevision &&
        current->bindingRevision == m_materialDropHover->bindingRevision)
        return current->runtimeIndex;
    return std::nullopt;
}
