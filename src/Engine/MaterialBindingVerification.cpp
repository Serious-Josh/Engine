#include "Engine/Engine.h"

#ifndef NDEBUG
#include "Core/Log.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "Scene/SceneSerializer.h"
#include "Renderer/Vertex.h"
#include "Renderer/ShaderData.h"
#include "assets/MaterialAssetSerializer.h"
#include "Project/ProjectPaths.h"
#include "DemoContent/FileBackedDemoScene.h"

void VerifyViewportMaterialDropIntegration(Engine& engine, FileBackedDemoScene& scene,
    ProjectAssetRegistry& registry, const std::array<std::optional<AssetID>, 4>& materials, const AssetID& freshMaterial);
void VerifyMaterialAdmission(Renderer&, Engine&, FileSceneAssetCatalog&, FileSceneRuntime&,
    const ProjectPaths&, ProjectAssetRegistry&);
void VerifyMaterialAdmissionShutdown(Renderer&, Engine&, FileSceneAssetCatalog&, FileSceneRuntime&,
    const ProjectPaths&, ProjectAssetRegistry&);

#include <Windows.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace
{
    void RequireBindings(bool condition, const std::string& message)
    {
        if(!condition) throw std::runtime_error(message);
    }
    using Microsoft::WRL::ComPtr;
    using Publication = SceneMaterialBindingPublication;
    using Status = Publication::Status;
}

// Diagnostic-only access to provenance and GPU readback. Tests use isolated
// project files, real production transactions, real submissions and debug-layer
// validation. Fixture setup/teardown waits are outside the publication path.
class SceneMaterialBindingVerification
{
public:
    static bool Run(Renderer& renderer, const Shader* vertex, const Shader* pixel, Engine& engine)
    {
        RequireBindings(renderer.ReleaseSceneResources(), "Cannot release initial diagnostic scene.");
        const auto root = std::filesystem::absolute(std::filesystem::path("build") /
            ("material-binding-fixture-" + std::to_string(GetCurrentProcessId())));
        const ProjectPaths paths(root);
        std::filesystem::create_directories(paths.GetMaterialsDirectory());
        std::filesystem::create_directories(paths.GetScenesDirectory());
        ProjectAssetRegistry registry;
        const auto textureID = AssetID::Generate();
        RequireBindings(textureID.has_value(), "Texture fixture identity failed.");
        std::filesystem::create_directories(root / "Textures");
        const auto texturePath = root / "Textures/streamed.tga";
        std::array<unsigned char, 18 + 16 * 16 * 3> texturePixels{};
        texturePixels[2] = 2;
        texturePixels[12] = texturePixels[14] = 16;
        texturePixels[16] = 24;
        std::fill(texturePixels.begin() + 18, texturePixels.end(), 91);
        {
            std::ofstream file(texturePath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(texturePixels.data()), texturePixels.size());
            RequireBindings(bool(file), "Cannot write isolated streaming texture.");
        }
        RequireBindings(registry.Register({*textureID, AssetType::Texture, "Textures/streamed.tga"}), "Texture registration failed.");
        std::array<std::string, 4> references;
        std::array<std::optional<AssetID>, 4> ids;
        std::string error;
        for(std::size_t i = 0; i < ids.size(); ++i)
        {
            ids[i] = AssetID::Generate();
            RequireBindings(ids[i].has_value(), "Material ID generation failed.");
            references[i] = ids[i]->ToReference();
            const auto relative = std::filesystem::path("Materials") / (std::to_string(i) + ".material");
            MaterialAssetData material;
            material.roughness = 0.2f + 0.15f * static_cast<float>(i);
            if(i == 0) material.baseColorTexture = textureID->ToReference();
            RequireBindings(MaterialAssetSerializer::SaveToFile(material, root / relative, &error), error);
            RequireBindings(registry.Register({*ids[i], AssetType::Material, relative}), "Material registration failed.");
        }
        FileBackedDemoScene owner(paths.GetScenesDirectory() / "drop-fixture.scene");
        owner.SetProjectAssets(root, registry);
        auto& catalog = owner.m_assets;
        auto& runtime = *owner.m_runtime;
        Mesh multi;
        FileSceneRuntime pendingRuntime;
        ComPtr<ID3D12Fence> gate;
        ComPtr<ID3D12Resource> readback;
        struct Cleanup
        {
            Renderer& renderer;
            FileSceneAssetCatalog& catalog;
            Mesh& mesh;
            ComPtr<ID3D12Fence>& gate;
            ~Cleanup()
            {
                if(gate) gate->Signal(1);
                if(!renderer.ReleaseSceneResources()) std::abort();
                mesh.Shutdown();
                catalog.Shutdown();
            }
        } cleanup{renderer, catalog, multi, gate};
        RequireBindings(catalog.Initialize(renderer.GetDevice(), renderer.GetResourceUploader(),
            renderer.GetResourceDescriptorHeap(), vertex, pixel), "Catalog initialization failed.");
        catalog.SetProjectAssets(paths, registry);
        catalog.SetTextureStreamingEnabled(true);
        std::array<Material*, 4> materials;
        for(std::size_t i = 0; i < materials.size(); ++i)
        {
            materials[i] = catalog.ResolveMaterial(references[i]);
            RequireBindings(materials[i] != nullptr, catalog.GetLastMaterialError());
        }
        const auto freshMaterial = AssetID::Generate();
        RequireBindings(freshMaterial.has_value() && MaterialAssetSerializer::SaveToFile({}, paths.GetMaterialsDirectory() /
            "fresh.material", &error) && registry.Register({*freshMaterial, AssetType::Material, "Materials/fresh.material"}), error);
        RequireBindings(catalog.ResolveFreshMaterial(freshMaterial->ToReference()) &&
            !catalog.FindRetainedMaterial(freshMaterial->ToReference()), "Fresh-only material fixture failed.");
        const std::array<Vertex, 3> vertices{{{{-1, 0, 0}, {0, 0, 1}, {0, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0.5f, 1}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}}};
        const std::array<unsigned int, 9> indices{0, 1, 2, 0, 1, 2, 0, 1, 2};
        RequireBindings(multi.Initialize(renderer.GetDevice(), renderer.GetResourceUploader(),
            renderer.GetResourceDescriptorHeap(), vertices.data(), vertices.size(), CreateVertexLayout(),
            indices.data(), indices.size()), "Fixture multi-slot mesh upload failed.");
        const MeshMetadata layout{{{0, 3, 0, "A"}, {3, 3, 1, "B"}, {6, 3, 2, "C"}},
            {{"a", "A", 0}, {"b", "B", 1}, {"c", "C", 2}}};
        RequireBindings(multi.SetMetadata(layout, error), error);
        struct Resolver final : SceneAssetResolver
        {
            Resolver(FileSceneAssetCatalog& assets, Mesh& mesh) : assets(assets), multi(mesh) {}
            FileSceneAssetCatalog& assets;
            Mesh& multi;
            Mesh* ResolveMesh(std::string_view reference) override
            { return reference == "builtin:cube" ? &multi : assets.ResolveMesh(reference); }
            Material* ResolveMaterial(std::string_view reference) override { return assets.ResolveMaterial(reference); }
        } resolver(catalog, multi);
        runtime.New("material-publication-fixture");
        auto& document = runtime.EditDocument();
        document.objects = {{"single", "Single", {}, "builtin:sphere", references[0], {}},
            {"multi", "Multi", {}, "builtin:cube", references[0],
                {{"a", references[0]}, {"b", references[1]}, {"c", references[2]}}}};
        RequireBindings(runtime.Prepare(resolver, &error) && runtime.Commit(&error), error);
        owner.m_initialized = true;
        RequireBindings(renderer.InitializeSceneResources(runtime.GetScene(), catalog.GetMaterials()),
            "Fixture renderer initialization failed.");
        auto& resources = renderer.m_rayTracingScene;
        auto& membership = *resources.m_membership;
        const auto geometryAddress = multi.GetVertexBufferAddress();
        const auto indexAddress = multi.GetIndexBufferAddress();
        const auto tlas = membership.topLevel.GetGPUAddress();
        const auto tlasRecording = membership.topLevel.GetRecordingIdentity();
        std::vector<ID3D12Resource*> blas;
        for(const auto& bottom : resources.m_blasRegistry.bottomLevels) blas.push_back(bottom->GetResource());
        ComPtr<ID3D12InfoQueue> info;
        renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&info));
        if(info) info->ClearStoredMessages();

        const auto prepare = [&](Publication& publication, const char* object,
            const std::map<std::string, std::string>& overrides)
        {
            const auto selection = runtime.GetObjectMetadata(object, &error);
            RequireBindings(selection.has_value(), error);
            const auto result = publication.Prepare(renderer, runtime, catalog, *selection, overrides);
            RequireBindings(result.status == Status::Ready, result.message);
        };
        const auto publish = [&](Publication& publication)
        {
            const auto submission = renderer.m_commandContext.GetLastSubmissionFenceValue();
            const auto result = publication.PublishAtBoundary(renderer, runtime, catalog);
            RequireBindings(result.status == Status::Ready && result.published, result.message);
            RequireBindings(submission == renderer.m_commandContext.GetLastSubmissionFenceValue(),
                "Publication unexpectedly submitted GPU commands.");
            RequireBindings(resources.ValidateMaterialBindings(runtime.GetScene()), "DXR snapshot diverged from raster bindings.");
            RequireBindings(membership.topLevel.GetGPUAddress() == tlas && membership.topLevel.GetRecordingIdentity() == tlasRecording &&
                multi.GetVertexBufferAddress() == geometryAddress &&
                multi.GetIndexBufferAddress() == indexAddress && multi.GetMetadata() == layout, "Geometry or TLAS changed.");
            for(std::size_t i = 0; i < blas.size(); ++i)
                RequireBindings(blas[i] == resources.m_blasRegistry.bottomLevels[i]->GetResource(), "BLAS changed.");
        };
        Publication publication;
        prepare(publication, "single", {{"default", references[3]}});
        publish(publication);
        RequireBindings(runtime.GetScene().GetRenderObjects()[0].GetSlotMaterial("default") == materials[3],
            "Single-slot replacement failed.");
        const auto before = runtime.GetObjectMetadata("multi")->materialSlotBindings;
        const std::map<std::string, std::string> allD{{"a", references[3]}, {"b", references[3]}, {"c", references[3]}};
        auto result = publication.PrepareAllSlots(renderer, runtime, catalog, *runtime.GetObjectMetadata("multi"), *ids[3]);
        RequireBindings(result.status == Status::Ready, result.message);
        publish(publication);
        RequireBindings(runtime.GetScene().GetRenderObjects()[1].slotMaterials.size() == 3 &&
            runtime.GetScene().GetRenderObjects()[1].GetSlotMaterial("b") == materials[3], "Multi-slot replacement failed.");
        prepare(publication, "multi", before);
        publish(publication);
        RequireBindings(runtime.GetScene().GetRenderObjects()[1].GetSlotMaterial("a") == materials[0] &&
            runtime.GetScene().GetRenderObjects()[1].GetSlotMaterial("b") == materials[1] &&
            runtime.GetScene().GetRenderObjects()[1].GetSlotMaterial("c") == materials[2], "Exact reversal failed.");
        prepare(publication, "multi", before);
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Ready && !result.published, "No-op published bindings.");
        RequireBindings(runtime.SaveAs(paths.GetScenesDirectory() / "roundtrip.scene", &error), error);
        SceneDocument restored;
        RequireBindings(LoadSceneDocument(paths.GetScenesDirectory() / "roundtrip.scene", restored, &error) &&
            restored.objects[1].materialSlotBindings == before, "Scene slot round trip failed.");

        prepare(publication, "multi", allD);
        RequireBindings(registry.Unregister(*ids[3]), "Fixture unregister failed.");
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Deleted material was accepted.");
        publication.Cancel();
        RequireBindings(registry.Register({*ids[3], AssetType::Material, "Materials/3.material"}), "Fixture restore failed.");
        prepare(publication, "multi", allD);
        FileSceneRuntime replacement;
        result = publication.PublishAtBoundary(renderer, replacement, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Scene replacement was accepted.");
        publication.Cancel();
        prepare(publication, "multi", allD);
        const auto savedLayout = multi.GetMetadata();
        auto changedLayout = savedLayout;
        changedLayout.materialSlots[0].name = "Changed layout";
        RequireBindings(multi.SetMetadata(changedLayout, error), error);
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Changed slot layout was accepted.");
        publication.Cancel();
        RequireBindings(multi.SetMetadata(savedLayout, error), error);
        prepare(publication, "multi", allD);
        runtime.GetScene().GetRenderObjects()[1].mesh = nullptr;
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Changed mesh residency was accepted.");
        runtime.GetScene().GetRenderObjects()[1].mesh = &multi;
        publication.Cancel();
        result = publication.Prepare(renderer, runtime, catalog, *runtime.GetObjectMetadata("multi"), {{"unknown-slot", references[3]}});
        RequireBindings(result.status == Status::Unsupported, "Unknown slot was accepted.");

        // Queue a real readback behind an unsignaled GPU gate. Every old lookup
        // must remain alive while several publications occur without waiting.
        renderer.m_rayTracingScene.CollectRetiredMemberships(renderer.m_commandContext);
        RequireBindings(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)) == S_OK,
            "Gate allocation failed.");
        RequireBindings(renderer.BeginTrackedRecording(0), "Readback recording failed.");
        const auto slotBytes = membership.slotMaterials.GetResource()->GetDesc().Width;
        D3D12_HEAP_PROPERTIES readHeap{};
        readHeap.Type = D3D12_HEAP_TYPE_READBACK;
        auto readDescription = membership.slotMaterials.GetResource()->GetDesc();
        readDescription.Width = slotBytes + runtime.GetScene().GetRenderObjects().size() * sizeof(RayTracingInstanceData);
        RequireBindings(SUCCEEDED(renderer.GetDevice()->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE,
            &readDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "Readback allocation failed.");
        auto* commands = renderer.m_commandContext.GetCommandList();
        // Active buffers are immutable upload tables after the preceding edits.
        commands->CopyBufferRegion(readback.Get(), 0, membership.slotMaterials.GetResource(), 0, slotBytes);
        commands->CopyBufferRegion(readback.Get(), slotBytes, membership.instanceDataBuffer.GetResource(), 0,
            readDescription.Width - slotBytes);
        RequireBindings(renderer.EndTrackedRecording(), "Readback close failed.");
        RequireBindings(SUCCEEDED(renderer.m_commandContext.GetCommandQueue()->Wait(gate.Get(), 1)), "Gate queue wait failed.");
        RequireBindings(renderer.ExecuteTrackedRecording(0), "Readback submission failed.");
        const auto heldFence = membership.lastUseFence;
        RequireBindings(renderer.m_commandContext.GetCompletedFenceValue() < heldFence, "Gate did not hold a real submission.");
        for(unsigned i = 0; i < 4; ++i)
        {
            prepare(publication, "multi", i % 2 ? before : allD);
            publish(publication);
        }
        prepare(publication, "multi", allD);
        const auto savedBindings = runtime.GetObjectMetadata("multi")->materialSlotBindings;
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Retry && !result.published &&
            runtime.GetObjectMetadata("multi")->materialSlotBindings == savedBindings, "Retirement pressure partially published.");
        RequireBindings(SUCCEEDED(gate->Signal(1)) && renderer.WaitForGPU(), "Readback completion failed.");
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readDescription.Width)};
        RequireBindings(SUCCEEDED(readback->Map(0, &readRange, &mapped)), "Readback map failed.");
        const auto* gpuSlots = static_cast<const unsigned int*>(mapped);
        RequireBindings(gpuSlots[0] == materials[3]->gpuIndex && gpuSlots[1] == materials[0]->gpuIndex &&
            gpuSlots[2] == materials[1]->gpuIndex && gpuSlots[3] == materials[2]->gpuIndex,
            "In-flight GPU lookup changed after later publications.");
        RayTracingInstanceData gpuInstance{};
        std::memcpy(&gpuInstance, static_cast<const char*>(mapped) + slotBytes + sizeof(RayTracingInstanceData), sizeof(gpuInstance));
        RequireBindings(gpuInstance.slotMaterialOffset == 1 && gpuInstance.materialIndex == materials[0]->gpuIndex,
            "GPU instance fallback/slot offsets disagree.");
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        resources.CollectRetiredMemberships(renderer.m_commandContext);
        publish(publication);
        RequireBindings(std::count_if(resources.m_retiredMaterialBindings.begin(), resources.m_retiredMaterialBindings.end(),
            [](const auto& value) { return bool(value); }) == 1, "Completed lookup resources were not retired.");

        RequireBindings(renderer.BeginTrackedRecording(1), "Published lookup recording failed.");
        commands = renderer.m_commandContext.GetCommandList();
        commands->CopyBufferRegion(readback.Get(), 0, membership.slotMaterials.GetResource(), 0, slotBytes);
        commands->CopyBufferRegion(readback.Get(), slotBytes, membership.instanceDataBuffer.GetResource(), 0,
            readDescription.Width - slotBytes);
        RequireBindings(renderer.EndTrackedRecording() && renderer.ExecuteTrackedRecording(1) && renderer.WaitForGPU(),
            "Published lookup readback failed.");
        RequireBindings(SUCCEEDED(readback->Map(0, &readRange, &mapped)), "Published lookup map failed.");
        gpuSlots = static_cast<const unsigned int*>(mapped);
        RequireBindings(gpuSlots[0] == materials[3]->gpuIndex && gpuSlots[1] == materials[3]->gpuIndex &&
            gpuSlots[2] == materials[3]->gpuIndex && gpuSlots[3] == materials[3]->gpuIndex,
            "Published GPU slot indices disagree with raster materials.");
        std::memcpy(&gpuInstance, static_cast<const char*>(mapped) + slotBytes + sizeof(RayTracingInstanceData), sizeof(gpuInstance));
        RequireBindings(gpuInstance.materialIndex == materials[3]->gpuIndex && gpuInstance.slotMaterialOffset == 1 &&
            gpuInstance.slotMaterialBufferIndex == membership.slotMaterials.GetDescriptorIndex() &&
            gpuInstance.triangleSlotBufferIndex == membership.triangleSlots.GetDescriptorIndex(),
            "Published GPU instance metadata references inconsistent tables.");
        readback->Unmap(0, &written);

        VerifyViewportMaterialDropIntegration(engine, owner, registry, ids, *freshMaterial);

        // Exercise the existing asynchronous streaming path with the source pin
        // retained across CPU completion, GPU upload and checked table publication.
        auto& texturePreparation = engine.GetTexturePreparationService();
        const auto textureStatistics = texturePreparation.GetStatistics();
        const auto* fallbackTexture = materials[0]->texture;
        bool observedPin = false;
        unsigned streamingFrames = 0;
        const auto textureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(materials[0]->texture == fallbackTexture && std::chrono::steady_clock::now() < textureDeadline)
        {
            if(texturePreparation.GetStatistics().workerHashes >= textureStatistics.workerHashes + 2)
            {
                const auto writable = CreateFileW(texturePath.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if(writable == INVALID_HANDLE_VALUE) observedPin = true;
                else CloseHandle(writable);
            }
            renderer.PollPreparedTextureUploads();
            catalog.AdvanceTextureStreaming(texturePreparation, renderer, runtime.GetRuntimeIdentity(), runtime.GetScene(),
                [](const AssetID&, const MaterialAssetData&) { return true; });
            const auto slot = streamingFrames++ % CommandContext::FrameCount;
            RequireBindings(renderer.BeginTrackedRecording(slot), "Streaming verification recording failed.");
            const auto generation = renderer.m_activeRecordingGeneration;
            const auto revision = renderer.m_materialPublication.revision;
            auto* list = renderer.m_commandContext.GetCommandList();
            renderer.RecordPreparedTextureUploads(list);
            RequireBindings(renderer.RecordMaterialTable(slot, list) && renderer.EndTrackedRecording() &&
                renderer.ExecuteTrackedRecording(slot), "Streaming verification submission failed.");
            const auto fence = renderer.m_commandContext.GetLastSubmissionFenceValue();
            for(auto& entry : renderer.m_preparedTextureUploads) entry.upload->ConfirmSubmission(generation, fence);
            RequireBindings(renderer.m_materialDependencies.Confirm(slot, fence), "Streaming dependency confirmation failed.");
            renderer.m_materialPublication.ConfirmSubmission(slot, revision);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        RequireBindings(materials[0]->texture != fallbackTexture && observedPin &&
            texturePreparation.GetStatistics().workerHashes >= textureStatistics.workerHashes + 2 &&
            renderer.m_materialPublication.desired[materials[0]->gpuIndex].textureIndex == materials[0]->texture->GetDescriptorIndex() &&
            resources.ValidateMaterialBindings(runtime.GetScene()), "Source-pinned texture streaming did not publish consistent material data.");
        const auto writable = CreateFileW(texturePath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        RequireBindings(writable != INVALID_HANDLE_VALUE, "Successful streaming publication retained the source pin.");
        CloseHandle(writable);
        Log::Info("Texture streaming source validation passed: worker evidence, in-flight source pin, GPU upload, material publication, pin release and unchanged object/DXR bindings.");
        VerifyMaterialAdmission(renderer, engine, catalog, runtime, paths, registry);

        // Reorder/delete identity checks operate on stable IDs, never a saved index.
        prepare(publication, "multi", before);
        const auto structure = runtime.m_objectMapping.structureRevision;
        ++runtime.m_objectMapping.structureRevision;
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Structural mutation was accepted.");
        runtime.m_objectMapping.structureRevision = structure;
        publication.Cancel();
        prepare(publication, "multi", before);
        const auto node = runtime.m_objectMapping.idToRuntimeIndex.extract("multi");
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Deleted target was accepted.");
        runtime.m_objectMapping.idToRuntimeIndex.emplace("multi", 1);
        publication.Cancel();
        prepare(publication, "multi", before);
        VerifyMaterialAdmissionShutdown(renderer, engine, catalog, runtime, paths, registry);
        result = publication.PublishAtBoundary(renderer, runtime, catalog);
        RequireBindings(result.status == Status::Stale && !result.published, "Renderer shutdown was accepted.");
        publication.Cancel();

        const auto pendingID = AssetID::Generate();
        RequireBindings(pendingID.has_value(), "Pending fixture ID failed.");
        const std::filesystem::path pendingPath = "Meshes/pending.fbx";
        std::filesystem::create_directories((root / pendingPath).parent_path());
        std::ofstream(root / pendingPath) << "Isolated metadata-only fixture; never imported.\n";
        RequireBindings(registry.Register({*pendingID, AssetType::Mesh, pendingPath}), "Pending registration failed.");
        catalog.SetTextureStreamingEnabled(true);
        pendingRuntime.New("pending-material-fixture");
        pendingRuntime.EditDocument().objects = {{"pending", "Pending", {}, pendingID->ToReference(), references[0], before}};
        RequireBindings(pendingRuntime.PrepareMetadata(catalog, &error) && pendingRuntime.CommitMetadata(catalog, &error), error);
        RequireBindings(renderer.InitializeSceneResources(pendingRuntime.GetScene(), catalog.GetMaterials()),
            "Pending renderer initialization failed.");
        const auto pendingSelection = pendingRuntime.GetObjectMetadata("pending");
        RequireBindings(pendingSelection.has_value(), "Pending metadata unavailable.");
        result = publication.Prepare(renderer, pendingRuntime, catalog, *pendingSelection, allD);
        RequireBindings(result.status == Status::Unsupported && !result.published &&
            pendingRuntime.GetObjectMetadata("pending")->materialSlotBindings == before &&
            !pendingRuntime.GetScene().GetRenderObjects()[0].mesh && !catalog.FindRetainedMesh(pendingID->ToReference()),
            "Pending override semantics changed or geometry was loaded.");
        if(info)
        {
            for(UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
            {
                SIZE_T bytes = 0;
                info->GetMessage(i, nullptr, &bytes);
                std::vector<char> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                info->GetMessage(i, message, &bytes);
                RequireBindings(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                    message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
            }
        }
        Log::Info("Material binding publication passed: single/multiple slots, reversal, no-op, save/load, invalid material/scene/layout/structure/object/residency, shutdown, pending overrides, old/new GPU lookup readback, retirement pressure and retry; unchanged geometry/BLAS/TLAS.");
        return true;
    }
};

bool Engine::VerifyMaterialBindingPublication()
{
    const auto restore = [&]()
    {
        auto* active = m_demoSceneManager.TryGetActiveScene();
        if(!active || !m_renderer.InitializeSceneResources(active->GetScene(), active->GetMaterials()))
            std::abort();
        m_renderedSceneStructureRevision = active->GetScene().GetStructureRevision();
    };
    try
    {
        const bool result = SceneMaterialBindingVerification::Run(m_renderer, m_demoVertexShader, m_demoPixelShader, *this);
        restore();
        return result;
    }
    catch(...)
    {
        restore();
        throw;
    }
}
#endif
