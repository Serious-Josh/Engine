#include "assets/DevelopmentCookedMeshProvider.h"
#include "Engine/Engine.h"

#ifndef NDEBUG
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Project/ProjectPaths.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"
#include "Scene/SceneSerializer.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/ProjectAssetResolver.h"
#include "imgui.h"

#include <Windows.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace
{
    void RequireLifecycle(bool valid, const std::string& error)
    {
        if(!valid) { throw std::runtime_error("Cooked lifecycle: " + error); }
    }

    void WriteLifecycleJSON(const std::filesystem::path& path, const nlohmann::ordered_json& value)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << value.dump(2) << '\n';
        stream.close();
        RequireLifecycle(bool(stream), "Cannot write isolated fixture JSON.");
    }
}

bool Engine::VerifyCookedMeshLifecycle(const std::filesystem::path& scenePath)
{
    namespace fs = std::filesystem;
    using Status = ProjectMeshPreparationService::Status;
    using CandidateStatus = RayTracingSceneResources::CandidateStatus;
    using PublicationStatus = MeshResidencyPublication::Status;
    using Json = nlohmann::ordered_json;

    auto* original = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    RequireLifecycle(original != nullptr, "No active file scene.");
    const auto originalRoot = original->GetProjectRoot();
    const auto actual = DevelopmentCookedMesh::Provider::Open(originalRoot, {});
    RequireLifecycle(bool(actual.provider), "Cannot open actual cooked mapping.");
    SceneDocument document;
    std::string error;
    RequireLifecycle(LoadSceneDocument(scenePath, document, &error), error);
    const auto found = std::find_if(document.objects.begin(), document.objects.end(), [&](const auto& object)
    {
        const auto id = AssetID::ParseReference(object.meshReference);
        return id && actual.provider->Lookup(*id).status == DevelopmentCookedMesh::Status::Success;
    });
    RequireLifecycle(found != document.objects.end(), "Scene has no matching mesh for lifecycle verification.");
    auto primary = *found;
    primary.materialReference = "builtin:default";
    primary.materialSlotBindings.clear();
    auto secondary = primary;
    secondary.id += "-lifecycle-copy";
    document.objects = {primary, secondary};
    document.id = "c2-lifecycle-fixture";

    const auto asset = *AssetID::ParseReference(primary.meshReference);
    const auto payload = actual.provider->Acquire(asset);
    RequireLifecycle(bool(payload.lease), "Cannot acquire immutable lifecycle payload.");
    const auto materials = m_projectAssetRegistry.GetAssets(AssetType::Material);
    RequireLifecycle(!materials.empty(), "Lifecycle fixture requires a registered material identity.");
    const auto materialID = materials.front().id;
    const auto slotID = payload.lease->Metadata()->metadata.materialSlots.front().id;
    document.objects.front().materialSlotBindings[slotID] = materialID.ToReference();

    const auto fixture = fs::absolute(fs::path("build/Debug") /
        ("cooked-lifecycle-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    const ProjectPaths fixturePaths(fixture);
    RequireLifecycle(fixturePaths.EnsureDirectories(&error), error);
    const auto fixtureScene = fixturePaths.GetScenesDirectory() / "Lifecycle.scene";
    RequireLifecycle(SaveSceneDocument(document, fixtureScene, &error), error);
    const auto* meshRecord = m_projectAssetRegistry.Find(asset);
    RequireLifecycle(meshRecord != nullptr, "Mesh registration disappeared.");
    const auto meshSource = ProjectAssetResolver::Resolve(ProjectPaths(originalRoot), m_projectAssetRegistry, asset);
    RequireLifecycle(meshSource.status == AssetAvailability::Available, "Fixture FBX source unavailable.");
    fs::create_directories((fixture / meshRecord->relativePath).parent_path());
    fs::copy_file(meshSource.resolvedPath, fixture / meshRecord->relativePath);
    const auto materialPath = fixture / materials.front().relativePath;
    fs::create_directories(materialPath.parent_path());
    RequireLifecycle(MaterialAssetSerializer::SaveToFile({}, materialPath, &error), error);

    std::ifstream manifestStream(originalRoot / "Cooked" / actual.provider->Mapping().publishedGeneration / "mesh-manifest.json");
    auto manifest = Json::parse(manifestStream);
    std::erase_if(manifest["assets"].get_ref<Json::array_t&>(), [&](const auto& entry)
    {
        return entry.at("assetID").template get<std::string>() != asset.ToString();
    });
    RequireLifecycle(manifest["assets"].size() == 1, "Fixture manifest selection differs.");
    unsigned generation = 0;
    const auto publishGeneration = [&]()
    {
        const auto name = "generations/generation-0-0-" + std::to_string(++generation);
        const auto directory = fixture / "Cooked" / name;
        fs::create_directories(directory / "Meshes");
        fs::copy_file(originalRoot / "Cooked" / actual.provider->Mapping().publishedGeneration /
            manifest["assets"][0]["payloadPath"].get<std::string>(),
            directory / manifest["assets"][0]["payloadPath"].get<std::string>());
        const auto manifestPath = directory / "mesh-manifest.json";
        WriteLifecycleJSON(manifestPath, manifest);
        MaterialThumbnailValidation validation;
        const Json activation = {{"schemaVersion", 1}, {"generation", name}, {"manifestSHA256", validation.Digest(manifestPath)}};
        const auto pending = fixture / "Cooked/pending-activation.json";
        const auto active = fixture / "Cooked/active-mesh-generation.json";
        WriteLifecycleJSON(pending, activation);
        RequireLifecycle(MoveFileExW(pending.c_str(), active.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot atomically replace isolated activation pointer.");
    };
    publishGeneration();

    // The manager borrows Engine's registry throughout. Never mutate user project
    // generations/materials/scenes; fixture files are retained for inspection.
    struct RestoreProject
    {
        DemoSceneManager& manager;
        fs::path root;
        const ProjectAssetRegistry& registry;
        ~RestoreProject() { manager.SetProjectAssets(root, registry); }
    } restore{m_demoSceneManager, originalRoot, m_projectAssetRegistry};
    m_demoSceneManager.SetProjectAssets(fixture, m_projectAssetRegistry);
    FileBackedDemoScene* scene = nullptr;
    std::shared_ptr<const DevelopmentCookedMesh::Provider> provider;
    SceneObjectMetadata selection;
    ProjectMeshPreparationService::Completion completion;
    FileSceneAssetCatalog::CookedPreparationResult prepared;

    const auto reload = [&]()
    {
        CancelMeshResidencyAcceptance();
        completion = {};
        prepared = {};
        RequireLifecycle(m_demoSceneManager.RequestMetadataOnlyFileScene(fixtureScene, &error), error);
        if(!ActivateRequestedFileScene()) { std::abort(); }
        scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        RequireLifecycle(scene && scene->GetProjectRoot() == fixture, "Isolated runtime did not activate.");
        m_automaticMeshResidency = {};
        m_automaticResidencyIntent = 0;
        scene->m_runtime->m_cookedPublicationAuthorized = true;
        provider = DevelopmentCookedMesh::Provider::Open(fixture, {}).provider;
        RequireLifecycle(bool(provider), "Replacement provider unavailable.");
        const auto metadata = scene->GetObjectMetadata(primary.id, &error);
        RequireLifecycle(bool(metadata), error);
        selection = *metadata;
    };
    const auto prepareCatalog = [&]()
    {
        const auto request = m_meshPreparation.RequestCooked({provider, asset, m_meshPreparation.Generation()}, m_meshCPUCache);
        RequireLifecycle(request.status == Status::Accepted, request.error);
        m_cookedPublicationTicket = request.ticket;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do
        {
            completion = m_meshPreparation.PollCooked(request.ticket);
            if(completion.status != Status::Pending) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while(std::chrono::steady_clock::now() < deadline);
        RequireLifecycle(completion.status == Status::Available, "CPU completion failed: " + completion.error);
        RequireLifecycle(m_meshPreparation.IntegrateCache(completion, m_meshCPUCache, error), error);
        prepared = scene->m_assets.ResolvePreparedCookedMesh(selection.meshReference, completion,
            m_meshPreparation, m_renderer, *scene->m_runtime);
        RequireLifecycle(prepared.status == Status::Available && prepared.candidate, prepared.error);
    };
    const auto prepareRenderer = [&]()
    {
        prepareCatalog();
        m_diagnosticMeshToken = scene->m_runtime->PrepareMeshResidency(selection, scene->m_assets, *prepared.candidate, &error);
        RequireLifecycle(bool(m_diagnosticMeshToken), error);
        m_diagnosticRendererLifetime = m_renderer.GetMaterialTableIdentity();
        RequireLifecycle(PrepareMeshResidencyAcceptance(*m_diagnosticMeshToken, error), error);
    };
    const auto waitReady = [&]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        CandidateStatus status;
        do
        {
            status = m_renderer.PollDXRMembershipCandidate(*m_meshResidencyPublication.m_candidate,
                *scene->m_runtime, scene->m_assets, error);
            if(status != CandidateStatus::Pending) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while(std::chrono::steady_clock::now() < deadline);
        RequireLifecycle(status == CandidateStatus::Ready, "Renderer candidate not ready: " + error);
    };
    const auto expectStale = [&](const char* label)
    {
        const auto result = m_meshResidencyPublication.PublishAtBoundary(m_renderer, *scene->m_runtime, scene->m_assets);
        RequireLifecycle(result.status == PublicationStatus::Stale && !result.published &&
            !scene->GetScene().GetRenderObjects()[selection.runtimeIndex].mesh, std::string(label) + " published a stale binding.");
        CancelMeshResidencyAcceptance();
        Log::Info(std::string("Cooked lifecycle passed: ") + label);
    };

    reload();
    prepareCatalog();
    const auto oldLease = provider->Acquire(asset);
    const auto oldGeometry = completion.geometry;
    publishGeneration();
    RequireLifecycle(!scene->m_assets.ValidateMeshResidencyCandidate(*prepared.candidate, selection.meshReference, &error),
        "Catalog-prepared old generation remained eligible.");
    RequireLifecycle(CookedMesh::Decode(oldLease.lease->Bytes(), {}).status == CookedMesh::Status::Success,
        "Immutable payload lease did not survive replacement.");
    reload();
    prepareCatalog();
    RequireLifecycle(completion.cacheHit && completion.geometry == oldGeometry, "New generation did not revalidate/reuse exact payload.");
    Log::Info("Cooked lifecycle passed: catalog-stage generation replacement, immutable lease survival and new-generation cache reuse.");

    for(const bool ready : {false, true})
    {
        reload();
        prepareRenderer();
        if(ready) { waitReady(); }
        publishGeneration();
        expectStale(ready ? "generation replacement at final acceptance" : "generation replacement after renderer candidate construction");
    }

    // Real pending-object material edit, followed by an ABA rollback. The old
    // authored reference returns, but its binding epoch must never return.
    for(const bool rollback : {false, true})
    {
        reload();
        prepareRenderer();
        waitReady();
        const auto assignment = scene->PrepareMaterialAssignment(selection, materialID.ToReference(), &error);
        RequireLifecycle(assignment && !assignment->noOp, "Material mutation not staged: " + error);
        auto* material = scene->m_assets.FindRetainedMaterial(materialID.ToReference());
        RequireLifecycle(material && scene->CommitMaterialAssignment(*assignment, *material, &error), error);
        if(rollback) { RequireLifecycle(scene->RollbackMaterialAssignment(*assignment, &error), error); }
        else { RequireLifecycle(scene->FinishMaterialAssignment(*assignment, &error), error); }
        const auto changed = scene->GetObjectMetadata(primary.id, &error);
        RequireLifecycle(changed && changed->bindingRevision > selection.bindingRevision, "Binding epoch did not advance.");
        expectStale(rollback ? "material ABA binding epoch" : "material assignment change");
    }

    reload();
    prepareRenderer();
    waitReady();
    SceneHierarchySnapshot hierarchy;
    RequireLifecycle(scene->BuildHierarchySnapshot(hierarchy, &error), error);
    std::vector<std::string> order;
    for(const auto& entry : hierarchy.entries) { order.push_back(entry.objectID); }
    auto reversed = order;
    std::reverse(reversed.begin(), reversed.end());
    RequireLifecycle(scene->ReorderObjects({selection.sceneID, selection.runtimeIdentity, selection.structureRevision}, order, reversed, &error), error);
    waitReady(); // Document-only hierarchy changes preserve runtime index/identity.
    Log::Info("Cooked lifecycle passed: hierarchy reorder preserves valid runtime membership identity.");
    CancelMeshResidencyAcceptance();

    reload();
    // Establish the normal editor history session with this isolated project's
    // paths. Engine's ordinary Frame UI intentionally targets the default project.
    auto& debugUI = m_renderer.GetDebugUI();
    debugUI.BeginFrame();
    m_editor.PrepareWorkspace(*scene, m_demoSceneManager, debugUI);
    Renderer::FinalSceneImage image{};
    RequireLifecycle(m_renderer.TryGetFinalSceneImage(image), "No editor scene image.");
    m_editor.Draw(*scene, m_demoSceneManager, debugUI, image.srv.ptr, image.width, image.height,
        0.0f, fixturePaths, m_projectAssetRegistry, m_renderer.GetMaterialPreviewImage());
    ImGui::EndFrame();
    prepareRenderer();
    RequireLifecycle(RequestSceneObjectDeletion(selection, error) && ExecuteSceneObjectDeletion(), error);
    RequireLifecycle(m_lastSceneObjectDeletion && m_lastSceneObjectDeletion->succeeded &&
        !m_meshResidencyPublication.HasCandidate() && !scene->GetObjectMetadata(primary.id),
        "Object deletion did not cancel the candidate and remove the binding.");
    Log::Info("Cooked lifecycle passed: object deletion before publication and checked renderer cleanup.");

    reload();
    auto transform = scene->GetScene().GetRenderObjects()[selection.runtimeIndex].transform;
    transform.position.x += 0.125f;
    scene->GetScene().SetRenderObjectTransform(selection.runtimeIndex, transform);
    prepareRenderer();
    waitReady();
    transform.position.x += 0.25f;
    scene->GetScene().SetRenderObjectTransform(selection.runtimeIndex, transform);
    const auto refreshed = m_meshResidencyPublication.PublishAtBoundary(m_renderer, *scene->m_runtime, scene->m_assets);
    RequireLifecycle(refreshed.status == PublicationStatus::Retry && !refreshed.published &&
        refreshed.message.find("transforms refreshed") != std::string::npos, "Transform-only change did not submit a retryable TLAS refresh.");
    waitReady();
    const auto accepted = m_meshResidencyPublication.PublishAtBoundary(m_renderer, *scene->m_runtime, scene->m_assets);
    RequireLifecycle(accepted.published, "Refreshed transforms were not accepted: " + accepted.message);
    const auto revision = scene->m_runtime->GetResidencyRevision();
    const auto duplicate = m_meshResidencyPublication.PublishAtBoundary(m_renderer, *scene->m_runtime, scene->m_assets);
    RequireLifecycle(!duplicate.published && scene->m_runtime->GetResidencyRevision() == revision &&
        scene->GetScene().GetRenderObjects()[selection.runtimeIndex].transform.position.x == transform.position.x,
        "Duplicate adoption or lost transform.");
    CancelMeshResidencyAcceptance();
    Log::Info("Cooked lifecycle passed: transforms during preparation, post-candidate TLAS refresh and duplicate-adoption exclusion.");

    // Resident slot edits use the actual checked editor/renderer transaction.
    const auto resident = scene->GetObjectMetadata(primary.id, &error);
    const auto secondarySelection = scene->GetObjectMetadata(secondary.id, &error);
    RequireLifecycle(bool(secondarySelection), error);
    selection = *secondarySelection;
    prepareRenderer();
    waitReady();
    RequireLifecycle(resident && RequestMaterialSlotAssignment(*resident, slotID, std::nullopt, error) && ExecuteMaterialAssignment(), error);
    const auto edited = scene->GetObjectMetadata(primary.id, &error);
    RequireLifecycle(edited && edited->bindingRevision > resident->bindingRevision && !edited->materialSlotBindings.contains(slotID),
        "Resident material-slot override removal failed.");
    expectStale("resident material-slot mutation invalidates pending membership through checked renderer rebuild");

    // Switching with a candidate also exercises collection of canceled BLAS and
    // membership borrows before catalog destruction. Leave a final pending
    // candidate for main's normal checked Engine::Shutdown path.
    reload();
    prepareRenderer();
    reload();
    RequireLifecycle(!m_meshResidencyPublication.HasCandidate(), "Scene replacement retained an adoptable candidate.");
    Log::Info("Cooked lifecycle passed: repeated reload and scene replacement with pending renderer work.");

    // Restore ordinary activation and let the existing FBX scheduler settle a
    // real authored binding. Cooked choice and exclusion must not leak sessions.
    m_demoSceneManager.SetProjectAssets(originalRoot, m_projectAssetRegistry);
    RequireLifecycle(m_demoSceneManager.RequestMetadataOnlyFileScene(scenePath, &error), error);
    if(!ActivateRequestedFileScene()) { std::abort(); }
    auto* ordinary = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    RequireLifecycle(ordinary && !ordinary->m_runtime->m_cookedPublicationAuthorized && m_automaticMeshResidency.enabled,
        "Diagnostic cooked selection/exclusion leaked into ordinary activation.");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Mesh* ordinaryMesh = nullptr;
    do
    {
        Frame();
        const auto metadata = ordinary->GetObjectMetadata(primary.id, &error);
        if(metadata) { ordinaryMesh = ordinary->GetScene().GetRenderObjects()[metadata->runtimeIndex].mesh; }
        if(ordinaryMesh || m_window.ShouldClose()) { break; }
    } while(std::chrono::steady_clock::now() < deadline);
    const auto ordinaryReceipt = ordinaryMesh ? ordinary->m_assets.DescribeMeshResidencyCandidate(primary.meshReference, *ordinaryMesh, &error) : std::nullopt;
    RequireLifecycle(ordinaryReceipt && ordinaryReceipt->sourceKind == MeshResidencyCandidate::SourceKind::ProjectAsset &&
        std::get<5>(ordinaryReceipt->key) == MeshPreparationSourceKind::FBX, "Ordinary activation did not publish through FBX: " + error);
    Log::Info("Cooked lifecycle passed: ordinary metadata-first activation restores automatic FBX residency and the cooked gate.");
    m_demoSceneManager.SetProjectAssets(fixture, m_projectAssetRegistry);
    reload();

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    RequireLifecycle(SUCCEEDED(m_renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))), "No D3D12 validation queue.");
    for(UINT64 index = 0; index < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T bytes = 0;
        RequireLifecycle(SUCCEEDED(queue->GetMessage(index, nullptr, &bytes)), "Cannot size validation message.");
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        RequireLifecycle(SUCCEEDED(queue->GetMessage(index, message, &bytes)), "Cannot read validation message.");
        RequireLifecycle(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
            std::string(message->pDescription, message->DescriptionByteLength));
    }
    prepareRenderer();
    Log::Info("Cooked lifecycle: no D3D12 error/corruption messages; pending renderer candidate retained for checked shutdown. Fixture: " + fixture.string());
    return true;
}
#endif
