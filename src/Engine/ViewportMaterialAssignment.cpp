#include "Engine/Engine.h"
#include "DemoContent/FileBackedDemoScene.h"

void Engine::ReleaseViewportMaterialPreparation() noexcept
{
    if(!m_pendingViewportMaterialBinding) return;
    auto& pending = *m_pendingViewportMaterialBinding;
    if(pending.admissionStarted) m_renderer.CancelMaterialAdmission();
    pending.admissionStarted = false;
    if(m_materialPreparation && pending.preparationTicket) m_materialPreparation->Cancel(pending.preparationTicket);
    pending.preparationTicket = 0;
    pending.candidate.reset();
}

bool Engine::PrepareViewportDropMaterial(FileBackedDemoScene& owner)
{
    using Preparation = ProjectMaterialPreparationService::Status;
    using Admission = MaterialAdmissionStatus;
    auto& pending = *m_pendingViewportMaterialBinding;
    if(!pending.request.allSlotsMaterial || pending.materialReady) return true;
    const auto material = *pending.request.allSlotsMaterial;
    const auto* record = pending.registry->Find(material);
    if(!record || record->type != AssetType::Material)
    {
        CancelViewportMaterialAssignment("Material is no longer registered.");
        return false;
    }
    if(owner.IsAssignmentMaterialAdmitted(material.ToReference(), m_renderer))
    {
        ReleaseViewportMaterialPreparation();
        pending.materialReady = true;
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if(now < pending.retryAfter) return false;
    const auto waiting = [&](std::string_view message, bool retry)
    {
        if(retry) pending.retryAfter = now + std::chrono::milliseconds(100);
        if(now >= pending.progressAfter)
        {
            m_editor.NotifyViewportMaterialRetry(pending.request, message);
            pending.progressAfter = now + std::chrono::milliseconds(100);
        }
        return false;
    };
    if(!m_materialPreparation)
        m_materialPreparation = std::make_unique<ProjectMaterialPreparationService>(m_texturePreparation);
    const ProjectPaths paths(pending.project);
    std::string error;
    if(!pending.preparationTicket)
    {
        const auto status = m_materialPreparation->Request(paths, *pending.registry, material, owner.GetRuntimeIdentity(),
            pending.preparationTicket, error);
        if(status == Preparation::Retry) return waiting(error, true);
        if(status != Preparation::Pending)
        {
            CancelViewportMaterialAssignment(error);
            return false;
        }
    }
    if(!pending.candidate)
    {
        auto result = m_materialPreparation->Poll(pending.preparationTicket, paths, *pending.registry, owner.GetRuntimeIdentity());
        if(result.status == Preparation::Pending) return waiting("Preparing material and textures...", false);
        if(result.status == Preparation::Retry) return waiting(result.message, true);
        if(result.status != Preparation::Ready)
        {
            CancelViewportMaterialAssignment(result.message);
            return false;
        }
        pending.candidate = std::move(result.candidate);
    }
    pending.admissionStarted = true;
    const auto result = m_renderer.AdvanceMaterialAdmission(*m_materialPreparation, pending.candidate, owner.m_assets,
        paths, *pending.registry, *owner.m_runtime);
    if(result.status == Admission::Pending) return waiting("Uploading material textures...", false);
    if(result.status == Admission::Retry) return waiting(result.message, true);
    if(result.status == Admission::Stale && m_materialPreparation->Validate(*pending.candidate, paths, *pending.registry,
        owner.GetRuntimeIdentity(), error))
    {
        // Unrelated scene texture publication can supersede the table generation.
        // Restart GPU staging only while the CPU evidence and captured owners remain valid.
        m_renderer.CancelMaterialAdmission();
        pending.admissionStarted = false;
        return waiting("Material table changed; retrying admission...", true);
    }
    if(result.status != Admission::Ready || !result.material || !m_renderer.HasPublishedMaterial(*result.material))
    {
        CancelViewportMaterialAssignment(result.message);
        return false;
    }
    ReleaseViewportMaterialPreparation(); // Acceptance is complete; release CPU source pins before binding.
    pending.materialReady = true;
    return true;
}

void Engine::CancelViewportMaterialAssignment(std::string_view reason) noexcept
{
    m_viewportMaterialPublication.Cancel();
    ReleaseViewportMaterialPreparation();
    if(m_pendingViewportMaterialBinding)
        m_editor.CompleteViewportMaterialRequest(m_pendingViewportMaterialBinding->request, false, false, reason);
    m_pendingViewportMaterialBinding.reset();
}

void Engine::ProcessViewportMaterialAssignment(IDemoScene& scene)
{
    using Status = SceneMaterialBindingPublication::Status;
    try
    {
        if(auto request = m_editor.TakeViewportMaterialRequest())
        {
            CancelViewportMaterialAssignment("A newer material drop superseded the pending assignment.");
            m_pendingViewportMaterialBinding.emplace(PendingViewportMaterialBinding{
                std::move(*request), std::chrono::steady_clock::now(), false});
        }
        if(!m_pendingViewportMaterialBinding) return;
        auto& pending = *m_pendingViewportMaterialBinding;
        auto* owner = dynamic_cast<FileBackedDemoScene*>(&scene);
        if(!owner || !m_editor.IsViewportMaterialRequestCurrent(pending.request) ||
            owner->GetRuntimeIdentity() != pending.request.selection.runtimeIdentity ||
            m_demoSceneManager.HasPendingSceneChange() || m_pendingEditorSceneOperation || m_pendingMaterialAssignment)
        {
            CancelViewportMaterialAssignment("Scene/session changed or a conflicting operation is active.");
            return;
        }
        // One bounded request, no blocking waits and no indefinite retries.
        if(std::chrono::steady_clock::now() - pending.started > std::chrono::seconds(10))
        {
            CancelViewportMaterialAssignment("Renderer remained busy for ten seconds. Try the drop again.");
            return;
        }
        const auto target = owner->GetObjectMetadata(pending.request.selection.objectID);
        const auto* mesh = target ? scene.GetScene().GetRenderObjects()[target->runtimeIndex].mesh : nullptr;
        const auto& expected = pending.request.selection;
        if(!mesh || mesh->GetMetadata() != pending.request.layout || target->sceneID != expected.sceneID ||
            target->structureRevision != expected.structureRevision || target->bindingRevision != expected.bindingRevision ||
            target->meshReference != expected.meshReference || target->materialReference != expected.materialReference ||
            target->materialSlotBindings != expected.materialSlotBindings)
        {
            CancelViewportMaterialAssignment("Target disappeared or its resident slot layout changed.");
            return;
        }
        if(!pending.registry)
        {
            if(!owner->m_registry) { CancelViewportMaterialAssignment("Project registry is unavailable."); return; }
            pending.registry = owner->m_registry;
            pending.project = std::filesystem::canonical(owner->GetProjectRoot());
            pending.rendererIdentity = m_renderer.GetMaterialTableIdentity();
        }
        if(pending.registry != owner->m_registry || pending.project != std::filesystem::canonical(owner->GetProjectRoot()) ||
            !pending.rendererIdentity || pending.rendererIdentity != m_renderer.GetMaterialTableIdentity())
        {
            CancelViewportMaterialAssignment("Project, registry or renderer identity changed.");
            return;
        }
        if(!PrepareViewportDropMaterial(*owner)) return;
        SceneMaterialBindingPublication::Result result;
        if(!pending.prepared)
        {
            result = pending.request.allSlotsMaterial ? m_viewportMaterialPublication.PrepareAllSlots(
                m_renderer, *owner->m_runtime, owner->m_assets, pending.request.selection, *pending.request.allSlotsMaterial) :
                m_viewportMaterialPublication.Prepare(m_renderer, *owner->m_runtime, owner->m_assets,
                    pending.request.selection, pending.request.overrides);
            pending.prepared = result.status == Status::Ready;
        }
        if(pending.prepared)
            result = m_viewportMaterialPublication.PublishAtBoundary(m_renderer, *owner->m_runtime, owner->m_assets);
        if(result.status == Status::Retry)
        {
            m_editor.NotifyViewportMaterialRetry(pending.request, result.message);
            return;
        }
        m_editor.CompleteViewportMaterialRequest(pending.request, result.status == Status::Ready, result.published, result.message);
        m_viewportMaterialPublication.Cancel();
        ReleaseViewportMaterialPreparation();
        m_pendingViewportMaterialBinding.reset();
    }
    catch(const std::exception& exception) { CancelViewportMaterialAssignment(exception.what()); }
    catch(...) { CancelViewportMaterialAssignment("Unexpected material publication failure."); }
}
