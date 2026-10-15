#include "assets/DevelopmentCookedMeshProvider.h"
#include "Engine/Engine.h"

#ifndef NDEBUG
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/MeshCookValidation.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <vector>

bool Engine::VerifyCookedMeshGPUPreparation(const std::filesystem::path& scenePath,
    std::shared_ptr<const CookedMeshSource::Provider> source)
{
    using Status = ProjectMeshPreparationService::Status;
    const auto reject = [](const std::string& error)
    {
        Log::Error("Cooked GPU preparation checkpoint failed: " + error);
        return false;
    };

    std::string error;
    if(!m_demoSceneManager.RequestMetadataOnlyFileScene(scenePath, &error)) { return reject(error); }

    // Reuse the normal checked scene switch; failure can leave renderer borrows
    // that must survive, just as in the interactive Engine path.
    if(!ActivateRequestedFileScene())
    {
        Log::Error("Cooked checkpoint scene activation could not prove GPU safety; aborting without teardown.");
        std::abort();
    }

    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    if(!scene || !scene->IsMetadataOnlyActivation()) { return reject("Metadata-first file scene did not activate."); }

    // This synchronous diagnostic never runs Update or the automatic scheduler.
    m_automaticMeshResidency = {};
    m_automaticResidencyIntent = 0;
    auto provider = source;
    if(!provider)
    {
        const auto opened = DevelopmentCookedMesh::Provider::Open(scene->GetProjectRoot(), {});
        if(!opened.provider) { return reject(std::string(opened.message)); }
        provider = opened.provider;
    }

    size_t verified = 0;
    for(const auto& record : m_projectAssetRegistry.GetAssets())
    {
        if(record.type != AssetType::Mesh) { continue; }
        const auto lookup = provider->Lookup(record.id);
        if(lookup.status == DevelopmentCookedMesh::Status::Missing) { continue; }
        if(lookup.status != DevelopmentCookedMesh::Status::Success) { return reject(std::string(lookup.message)); }

        const auto request = m_meshPreparation.RequestCooked(
            {provider, record.id, m_meshPreparation.Generation()}, m_meshCPUCache);
        if(request.status != Status::Accepted) { return reject(request.error); }

        auto completion = m_meshPreparation.PollCooked(request.ticket);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while(completion.status == Status::Pending && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            completion = m_meshPreparation.PollCooked(request.ticket);
        }

        if(completion.status != Status::Available)
        {
            m_meshPreparation.Cancel(request.ticket);
            return reject("CPU completion: " + completion.error);
        }

        const auto reference = record.id.ToReference();
        auto prepared = scene->m_assets.ResolvePreparedCookedMesh(reference, completion,
            m_meshPreparation, m_renderer, *scene->m_runtime);
        if(prepared.status != Status::Available || !prepared.mesh || !prepared.candidate)
        {
            m_meshPreparation.Cancel(request.ticket);
            return reject("Catalog GPU preparation: " + prepared.error);
        }

        const auto* mesh = prepared.mesh;
        const auto* geometry = scene->m_assets.GetMeshGeometry(*mesh);
        const auto& metadata = *completion.metadata;
        if(geometry != completion.geometry.get() || mesh->GetVertexCount() != metadata.vertexCount ||
            mesh->GetIndexCount() != metadata.indexCount || mesh->GetMetadata() != metadata.metadata ||
            !mesh->GetBounds().valid || !MeshCookValidation::Same(mesh->GetBounds().min, metadata.boundsMin) ||
            !MeshCookValidation::Same(mesh->GetBounds().max, metadata.boundsMax) ||
            !scene->m_assets.ValidateMeshResidencyCandidate(*prepared.candidate, reference, &error))
        {
            m_meshPreparation.Cancel(request.ticket);
            return reject("Upload-complete geometry/metadata receipt: " + error);
        }

        // Cancel authorization, not catalog ownership. No scene or renderer
        // membership adopts this borrow; shutdown drains before catalog release.
        const auto vertexAddress = mesh->GetVertexBufferAddress();
        const auto indexAddress = mesh->GetIndexBufferAddress();
        m_meshPreparation.Cancel(request.ticket);
        if(scene->m_assets.ValidateMeshResidencyCandidate(*prepared.candidate, reference, &error) ||
            scene->m_assets.GetMeshGeometry(*mesh) != geometry ||
            mesh->GetVertexBufferAddress() != vertexAddress || mesh->GetIndexBufferAddress() != indexAddress)
        {
            return reject("Cancellation did not revoke evidence while retaining catalog geometry.");
        }

        ++verified;
        Log::Info("Cooked GPU preparation verified: " + reference + ", vertices=" +
            std::to_string(metadata.vertexCount) + ", indices=" + std::to_string(metadata.indexCount) +
            "; synchronous upload complete, metadata/bounds match, cancellation retains ownership.");
    }

    if(!verified) { return reject("No registered published cooked mesh was available."); }
    Log::Info("Cooked GPU preparation checkpoint passed for " + std::to_string(verified) +
        " meshes; publication remains gated.");
    return true;
}

bool Engine::VerifyCookedMeshPublication(const std::filesystem::path& scenePath, bool visualReview,
    std::shared_ptr<const CookedMeshSource::Provider> source)
{
    // Repeat the preparation prerequisite in this process before opening any gate.
    if(!VerifyCookedMeshGPUPreparation(scenePath, source)) { return false; }

    const auto reject = [&](const std::string& error)
    {
        Log::Error("Cooked publication diagnostic failed: " + error);
        CancelMeshResidencyAcceptance();
        if(auto* active = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene()))
        {
            active->m_runtime->m_cookedPublicationAuthorized = false;
        }
        return false;
    };

    // Fresh runtime/catalog: the checkpoint deliberately canceled every receipt.
    std::string error;
    if(!m_demoSceneManager.RequestMetadataOnlyFileScene(scenePath, &error)) { return reject(error); }
    if(!ActivateRequestedFileScene()) { std::abort(); }
    auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    if(!scene || !scene->IsMetadataOnlyActivation()) { return reject("Metadata-first activation failed."); }

    m_automaticMeshResidency = {};
    m_automaticResidencyIntent = 0;
    scene->m_runtime->m_cookedPublicationAuthorized = true;
    auto provider = source;
    if(!provider)
    {
        const auto opened = DevelopmentCookedMesh::Provider::Open(scene->GetProjectRoot(), {});
        if(!opened.provider) { return reject(std::string(opened.message)); }
        provider = opened.provider;
    }

    std::optional<SceneObjectMetadata> selection;
    const auto& objects = scene->GetScene().GetRenderObjects();
    for(size_t index = 0; index < objects.size(); ++index)
    {
        auto metadata = scene->GetObjectMetadata(index, &error);
        const auto id = metadata ? AssetID::ParseReference(metadata->meshReference) : std::nullopt;
        if(id && metadata->GetMeshResidency(objects[index]) == SceneMeshResidency::Pending &&
            provider->Lookup(*id).status == DevelopmentCookedMesh::Status::Success)
        {
            selection = std::move(metadata);
            break;
        }
    }
    if(!selection) { return reject("No pending registered mesh has a published cooked payload; no FBX fallback."); }

    const auto request = [&]()
    {
        const auto id = AssetID::ParseReference(selection->meshReference);
        const auto cpu = m_meshPreparation.RequestCooked(
            {provider, *id, m_meshPreparation.Generation()}, m_meshCPUCache);
        if(cpu.status != ProjectMeshPreparationService::Status::Accepted) { error = cpu.error; return false; }

        m_residencyRequestSelection = *selection;
        m_residencyRequestAutomatic = false;
        m_diagnosticRendererLifetime = m_renderer.GetMaterialTableIdentity();
        m_diagnosticCPUPreparation.emplace();
        m_diagnosticCPUPreparation->selection = *selection;
        m_diagnosticCPUPreparation->ticket = cpu.ticket;
        m_diagnosticCPUPreparation->generation = m_meshPreparation.Generation();
        m_diagnosticCPUPreparation->cooked = true;
        return true;
    };

    // Exercise cancellation after checked renderer candidate submission. The
    // manager retains in-flight resources; the same catalog remains alive.
    if(!request()) { return reject(error); }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while(!m_meshResidencyPublication.HasCandidate() && std::chrono::steady_clock::now() < deadline)
    {
        ProcessMeshResidencyDiagnostics();
        if(!m_diagnosticCPUPreparation && !m_diagnosticMeshToken) { return reject("Preparation rejected before cancellation test."); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if(!m_meshResidencyPublication.HasCandidate()) { return reject("Renderer candidate preparation timed out."); }
    CancelMeshResidencyAcceptance();
    if(scene->GetScene().GetRenderObjects()[selection->runtimeIndex].mesh)
    {
        return reject("Canceled renderer candidate changed the pending binding.");
    }
    Log::Info("Cooked renderer candidate canceled; pending binding unchanged and GPU manager ownership retained.");

    if(!request()) { return reject(error); }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool published = false;
    while(!m_window.ShouldClose() && std::chrono::steady_clock::now() < deadline)
    {
        Frame(); // Existing pre-Render boundary performs all final acceptance.
        if(m_meshResidencyAcceptance && m_meshResidencyAcceptance->published) { published = true; break; }
        if(!m_diagnosticCPUPreparation && !m_diagnosticMeshToken && !m_meshResidencyPublication.HasCandidate())
        {
            return reject("Existing coordinator rejected the cooked request; see diagnostic outcome above.");
        }
    }
    if(!published) { return reject("Live publication timed out or window closed."); }
    if(m_demoSceneManager.TryGetActiveScene() != scene || scene->GetRuntimeIdentity() != selection->runtimeIdentity)
    {
        return reject("Active scene changed during the diagnostic.");
    }

    const auto resident = scene->GetObjectMetadata(selection->objectID, &error);
    const auto& object = scene->GetScene().GetRenderObjects()[selection->runtimeIndex];
    const auto cookedMetadata = provider->ReadMetadata(*AssetID::ParseReference(selection->meshReference));
    if(!resident || resident->GetMeshResidency(object) != SceneMeshResidency::Resident ||
        resident->materialReference != selection->materialReference || resident->materialSlotBindings != selection->materialSlotBindings ||
        !object.mesh || !cookedMetadata.metadata || object.mesh->GetMetadata() != cookedMetadata.metadata->metadata)
    {
        return reject("Published residency or authored material slots differ.");
    }

    // Submit real frames with the adopted membership, then switch through the
    // same checked drain/release path used by the editor before inspecting errors.
    auto lastFrame = m_renderer.GetCpuFrameTiming().frameSerial;
    size_t submittedFrames = 0;
    for(size_t frame = 0; frame < 8 && !m_window.ShouldClose(); ++frame)
    {
        Frame();
        const auto timing = m_renderer.GetCpuFrameTiming();
        if(timing.submitted && timing.frameSerial != lastFrame)
        {
            ++submittedFrames;
            lastFrame = timing.frameSerial;
        }
    }
    if(m_window.ShouldClose()) { return reject("Window closed or a rendered frame failed."); }
    if(!submittedFrames) { return reject("No adopted-membership frame submission was observed."); }
    if(visualReview)
    {
        Log::Info("Human visual review: cooked mesh is resident; inspect transforms, orientation, normals, material slots, shading, ray tracing and AO. Close the window to continue checked cleanup. No visual pass is inferred automatically.");
        Run();
    }
    if(!m_demoSceneManager.RequestMetadataOnlyFileScene(scenePath, &error)) { return reject(error); }
    if(!ActivateRequestedFileScene()) { std::abort(); }
    m_automaticMeshResidency = {};
    m_automaticResidencyIntent = 0;

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    if(FAILED(m_renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        return reject("D3D12 validation queue unavailable.");
    }
    for(UINT64 index = 0; index < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T bytes = 0;
        if(FAILED(queue->GetMessage(index, nullptr, &bytes))) { return reject("Cannot inspect validation message."); }
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(FAILED(queue->GetMessage(index, message, &bytes))) { return reject("Cannot read validation message."); }
        if(message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            return reject(std::string(message->pDescription, message->DescriptionByteLength));
        }
    }

    Log::Info("Cooked live publication verified: " + selection->meshReference +
        "; BLAS/membership adoption, authored slots, " + std::to_string(submittedFrames) +
        " submitted frames, cancellation, scene switch and D3D12 validation passed.");
    return visualReview || bool(source) || VerifyCookedMeshLifecycle(scenePath);
}
#endif
