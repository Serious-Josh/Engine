#include "Engine/Engine.h"
#ifndef NDEBUG
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "assets/MaterialAssetSerializer.h"
#include <Windows.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace
{
    void CheckAdmission(bool condition, const std::string& message)
    { if(!condition) throw std::runtime_error("Material admission diagnostic: " + message); }
    using Status = MaterialAdmissionStatus;
    using Candidate = ProjectMaterialPreparationService::Candidate;
    using Clock = std::chrono::steady_clock;
}

class MaterialAdmissionVerification
{
public:
    static void Run(Renderer& renderer, Engine& engine, FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime,
        const ProjectPaths& paths, ProjectAssetRegistry& registry)
    {
        ProjectMaterialPreparationService preparation(engine.GetTexturePreparationService());
        unsigned frame = 0;
        const auto submit = [&]
        {
            const auto slot = frame++ % CommandContext::FrameCount;
            CheckAdmission(renderer.BeginTrackedRecording(slot), "Cannot begin test frame.");
            const auto generation = renderer.m_activeRecordingGeneration;
            const auto revision = renderer.m_materialPublication.revision;
            auto* commands = renderer.m_commandContext.GetCommandList();
            renderer.RecordPreparedTextureUploads(commands);
            CheckAdmission(renderer.RecordMaterialTable(slot, commands) && renderer.EndTrackedRecording() &&
                renderer.ExecuteTrackedRecording(slot), "Cannot submit test frame.");
            const auto fence = renderer.m_commandContext.GetLastSubmissionFenceValue();
            for(auto& upload : renderer.m_preparedTextureUploads) upload.upload->ConfirmSubmission(generation, fence);
            CheckAdmission(renderer.m_materialDependencies.Confirm(slot, fence), "Cannot confirm dependencies.");
            renderer.m_materialPublication.ConfirmSubmission(slot, revision);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            renderer.PollPreparedTextureUploads();
        };
        const auto settle = [&]
        {
            for(size_t slot = 0; slot < CommandContext::FrameCount; ++slot) submit();
            CheckAdmission(renderer.WaitForGPU(), "Cannot settle diagnostic frames.");
        };
        std::string error;
        const auto createMaterial = [&](const MaterialAssetData& data)
        {
            const auto id = *AssetID::Generate();
            const auto relative = std::filesystem::path("Materials") / (id.ToString() + ".material");
            CheckAdmission(MaterialAssetSerializer::SaveToFile(data, paths.GetRoot() / relative, &error) &&
                registry.Register({id, AssetType::Material, relative}), error);
            return id;
        };
        const auto createTexture = [&]
        {
            const auto id = *AssetID::Generate();
            const auto relative = std::filesystem::path("Textures") / (id.ToString() + ".tga");
            std::array<unsigned char, 18 + 16 * 16 * 3> image{};
            image[2] = 2; image[12] = image[14] = 16; image[16] = 24;
            { std::ofstream out(paths.GetRoot() / relative, std::ios::binary);
              out.write(reinterpret_cast<const char*>(image.data()), image.size());
              CheckAdmission(bool(out), "Cannot write texture fixture."); }
            CheckAdmission(registry.Register({id, AssetType::Texture, relative}), "Cannot register texture fixture.");
            return id;
        };
        std::uint64_t ticket = 0;
        const auto prepare = [&](const AssetID& id)
        {
            CheckAdmission(preparation.Request(paths, registry, id, runtime.GetRuntimeIdentity(), ticket, error) ==
                ProjectMaterialPreparationService::Status::Pending, error);
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while(Clock::now() < deadline)
            {
                auto result = preparation.Poll(ticket, paths, registry, runtime.GetRuntimeIdentity());
                if(result.status == ProjectMaterialPreparationService::Status::Ready) return result.candidate;
                CheckAdmission(result.status == ProjectMaterialPreparationService::Status::Pending ||
                    result.status == ProjectMaterialPreparationService::Status::Retry, result.message);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            throw std::runtime_error("CPU candidate timeout.");
        };
        const auto advance = [&](const std::shared_ptr<const Candidate>& candidate)
        { return renderer.AdvanceMaterialAdmission(preparation, candidate, catalog, paths, registry, runtime); };
        const auto admit = [&](const std::shared_ptr<const Candidate>& candidate)
        {
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while(Clock::now() < deadline)
            {
                auto result = advance(candidate);
                if(result.status == Status::Ready) return result.material;
                CheckAdmission(!result.material && (result.status == Status::Pending || result.status == Status::Retry), result.message);
                submit();
            }
            throw std::runtime_error("GPU admission timeout.");
        };
        const auto initialRecords = renderer.m_materialPublication.desired;
        const auto initialIdentities = renderer.m_publishedMaterialIdentities;
        const auto objects = runtime.GetScene().GetRenderObjects();
        const auto membership = renderer.m_rayTracingScene.m_membership.get();
        const auto tlas = membership->topLevel.GetGPUAddress();
        const auto originalCount = initialRecords.size();

        // Hold a real old-table use in flight. Admission must neither wait for it
        // nor release its slot's resource/descriptor. A second growth must defer.
        Microsoft::WRL::ComPtr<ID3D12Fence> gate;
        CheckAdmission(SUCCEEDED(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "Fence creation.");
        struct ReleaseGate { ID3D12Fence* gate; ~ReleaseGate() { gate->Signal(1); } } release{gate.Get()};
        settle();
        auto plainID = createMaterial({});
        auto plain = prepare(plainID);
        renderer.m_materialPublication.published[frame % CommandContext::FrameCount] = 0;
        renderer.m_commandContext.GetCommandQueue()->Wait(gate.Get(), 1);
        submit();
        const auto heldSlot = (frame - 1) % CommandContext::FrameCount;
        const auto oldBuffer = renderer.m_materialBuffers[heldSlot].GetResource();
        const auto submission = renderer.m_commandContext.GetLastSubmissionFenceValue();
        auto result = advance(plain);
        CheckAdmission(result.status == Status::Ready && result.material && result.material->gpuIndex == originalCount &&
            renderer.m_materialBuffers[heldSlot].GetResource() == oldBuffer &&
            renderer.m_commandContext.GetLastSubmissionFenceValue() == submission, "Admission changed in-flight storage or submitted GPU work.");
        auto* plainMaterial = result.material;
        auto secondID = createMaterial({});
        preparation.Cancel(ticket); plain.reset();
        auto second = prepare(secondID);
        CheckAdmission(advance(second).status == Status::Retry, "Pending table retirement did not defer growth.");
        gate->Signal(1);
        CheckAdmission(renderer.WaitForGPU(), "Cannot release held frame.");
        settle();

        // Descriptor pressure must reject before any renderer-visible mutation.
        std::vector<unsigned int> heldDescriptors;
        while(renderer.m_resourceDescriptorHeap.GetAvailableCount()) heldDescriptors.push_back(renderer.m_resourceDescriptorHeap.Allocate());
        struct FreeDescriptors
        {
            DescriptorHeap& heap; std::vector<unsigned int>& descriptors;
            ~FreeDescriptors() { for(auto descriptor : descriptors) heap.Free(descriptor); }
        } freeDescriptors{renderer.m_resourceDescriptorHeap, heldDescriptors};
        const auto beforePressure = renderer.m_materialPublication.revision;
        CheckAdmission(advance(second).status == Status::Retry && renderer.m_materialPublication.revision == beforePressure,
            "Descriptor pressure mutated table.");
        for(auto descriptor : heldDescriptors) renderer.m_resourceDescriptorHeap.Free(descriptor);
        heldDescriptors.clear(); renderer.CancelMaterialAdmission();
        auto* secondMaterial = admit(second); settle();
        CheckAdmission(secondMaterial->gpuIndex == originalCount + 1, "Append index changed.");
        const auto repeatedRevision = renderer.m_materialPublication.revision;
        CheckAdmission(advance(second).material == secondMaterial && renderer.m_materialPublication.revision == repeatedRevision,
            "Repeated admission was not idempotent.");
        preparation.Cancel(ticket); second.reset();

        auto colorID = createTexture();
        MaterialAssetData texturedData; texturedData.baseColorTexture = colorID.ToReference();
        auto texturedID = createMaterial(texturedData);
        auto textured = prepare(texturedID);
        // Saturate the existing upload queue using isolated, unbound requests.
        std::array<std::uint64_t, Renderer::kTextureUploadCapacity> occupied{};
        for(auto& upload : occupied) CheckAdmission(renderer.QueuePreparedTextureUpload(*textured->textures[0]->prepared, upload, error), error);
        CheckAdmission(advance(textured).status == Status::Retry, "Upload queue pressure did not defer admission.");
        renderer.CancelMaterialAdmission();
        for(auto upload : occupied) renderer.CancelPreparedTextureUpload(upload);
        renderer.PollPreparedTextureUploads();
        auto* texturedMaterial = admit(textured); settle();
        CheckAdmission(texturedMaterial->texture && texturedMaterial->texture->GetDescriptorIndex() != UINT_MAX,
            "Texture admitted before ready.");
        preparation.Cancel(ticket); textured.reset();

        auto detailID = createTexture();
        texturedData.normalTexture = detailID.ToReference();
        texturedData.roughnessTexture = texturedData.metallicTexture = detailID.ToReference();
        auto multipleID = createMaterial(texturedData);
        auto multiple = prepare(multipleID);
        auto* multipleMaterial = admit(multiple); settle();
        CheckAdmission(multipleMaterial->texture == texturedMaterial->texture &&
            multipleMaterial->roughnessTexture == multipleMaterial->metallicTexture &&
            multipleMaterial->normalTexture != multipleMaterial->roughnessTexture,
            "Texture dependency interpretation/sharing mismatch.");
        preparation.Cancel(ticket); multiple.reset();

        auto cachedID = createMaterial({});
        auto* cachedMaterial = catalog.ResolveMaterial(cachedID.ToReference()); // Diagnostic setup only.
        CheckAdmission(cachedMaterial && !renderer.HasPublishedMaterial(*cachedMaterial), "Unpublished cache fixture.");
        auto cachedCandidate = prepare(cachedID);
        CheckAdmission(admit(cachedCandidate) == cachedMaterial, "Cached material pointer was replaced during admission.");
        settle(); preparation.Cancel(ticket); cachedCandidate.reset();

        CheckAdmission(renderer.m_materialPublication.desired.size() == originalCount + 5 &&
            std::memcmp(initialRecords.data(), renderer.m_materialPublication.desired.data(), originalCount * sizeof(GPUMaterial)) == 0 &&
            std::equal(initialIdentities.begin(), initialIdentities.end(), renderer.m_publishedMaterialIdentities.begin()),
            "Existing material parameters or indices changed.");
        for(size_t index = 0; index < objects.size(); ++index)
            CheckAdmission(objects[index].material == runtime.GetScene().GetRenderObjects()[index].material &&
                objects[index].slotMaterials == runtime.GetScene().GetRenderObjects()[index].slotMaterials, "Admission changed object bindings.");
        CheckAdmission(renderer.m_rayTracingScene.m_membership.get() == membership &&
            membership->topLevel.GetGPUAddress() == tlas && renderer.m_rayTracingScene.ValidateMaterialBindings(runtime.GetScene()),
            "Admission changed DXR membership.");

        // Read both grown GPU tables, not merely their CPU desired snapshot.
        for(size_t slot = 0; slot < CommandContext::FrameCount; ++slot)
        {
            const auto bytes = renderer.m_materialPublication.desired.size() * sizeof(GPUMaterial);
            Microsoft::WRL::ComPtr<ID3D12Resource> readback;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = bytes; description.Height = 1; description.DepthOrArraySize = 1;
            description.MipLevels = 1; description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CheckAdmission(SUCCEEDED(renderer.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "Cannot allocate table readback.");
            CheckAdmission(renderer.BeginTrackedRecording(static_cast<unsigned>(slot)), "Readback recording.");
            renderer.m_commandContext.GetCommandList()->CopyBufferRegion(readback.Get(), 0,
                renderer.m_materialBuffers[slot].GetResource(), 0, bytes);
            CheckAdmission(renderer.EndTrackedRecording() && renderer.ExecuteTrackedRecording(static_cast<unsigned>(slot)) &&
                renderer.WaitForGPU(), "Table readback submission.");
            void* mapped = nullptr; D3D12_RANGE range{0, bytes};
            CheckAdmission(SUCCEEDED(readback->Map(0, &range, &mapped)), "Table readback map.");
            const bool equal = std::memcmp(mapped, renderer.m_materialPublication.desired.data(), bytes) == 0;
            const D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
            CheckAdmission(equal, "Grown GPU table differs from raster/DXR desired records.");
        }

        // The existing checked binding path can immediately resolve the new ID.
        SceneMaterialBindingPublication binding;
        const auto oldOverrides = runtime.GetObjectMetadata("multi")->materialSlotBindings;
        auto bound = binding.PrepareAllSlots(renderer, runtime, catalog, *runtime.GetObjectMetadata("multi"), multipleID);
        CheckAdmission(bound.status == SceneMaterialBindingPublication::Status::Ready, bound.message);
        bound = binding.PublishAtBoundary(renderer, runtime, catalog);
        CheckAdmission(bound.published && runtime.GetScene().GetRenderObjects()[1].GetSlotMaterial("a") == multipleMaterial,
            "Binding newly admitted material failed.");
        bound = binding.Prepare(renderer, runtime, catalog, *runtime.GetObjectMetadata("multi"), oldOverrides);
        CheckAdmission(bound.status == SceneMaterialBindingPublication::Status::Ready, bound.message);
        CheckAdmission(binding.PublishAtBoundary(renderer, runtime, catalog).published, "Cannot restore test bindings.");

        auto obsoleteID = createMaterial(texturedData);
        auto obsolete = prepare(obsoleteID);
        preparation.Cancel(ticket);
        CheckAdmission(advance(obsolete).status == Status::Stale, "Canceled CPU candidate accepted.");
        obsolete.reset();
        obsolete = prepare(obsoleteID);
        CheckAdmission(registry.Unregister(obsoleteID) && advance(obsolete).status == Status::Stale,
            "Material registry removal accepted.");
        preparation.Cancel(ticket); obsolete.reset();
        CheckAdmission(registry.Register({obsoleteID, AssetType::Material,
            std::filesystem::path("Materials") / (obsoleteID.ToString() + ".material")}), "Restore registry fixture.");
        obsolete = prepare(obsoleteID);
        CheckAdmission(registry.Unregister(detailID) && advance(obsolete).status == Status::Stale, "Stale texture candidate accepted.");
        preparation.Cancel(ticket); obsolete.reset();
        CheckAdmission(registry.Register({detailID, AssetType::Texture,
            std::filesystem::path("Textures") / (detailID.ToString() + ".tga")}), "Restore texture registry.");
        obsolete = prepare(obsoleteID);
        FileSceneRuntime anotherScene; anotherScene.New("admission-other-scene");
        CheckAdmission(renderer.AdvanceMaterialAdmission(preparation, obsolete, catalog, paths, registry, anotherScene).status == Status::Stale,
            "Scene replacement accepted.");
        preparation.Cancel(ticket); obsolete.reset();
        CheckAdmission(renderer.HasPublishedMaterial(*plainMaterial), "Existing admitted identity lost.");
        // Cancellation during a submitted upload cannot release staging or the
        // texture until its exact fence completes. Renderer polling notices CPU
        // revocation without another admission call from the coordinator.
        auto canceledTexture = createTexture();
        MaterialAssetData canceledData; canceledData.baseColorTexture = canceledTexture.ToReference();
        auto canceledID = createMaterial(canceledData);
        auto canceled = prepare(canceledID);
        const auto countBeforeCancel = renderer.m_materialPublication.desired.size();
        auto queued = advance(canceled);
        CheckAdmission(queued.status == Status::Pending && renderer.m_materialAdmission, "Cancellation fixture did not queue.");
        const auto uploadTicket = renderer.m_preparedTextureUploads.back().ticket;
        Microsoft::WRL::ComPtr<ID3D12Fence> uploadGate;
        CheckAdmission(SUCCEEDED(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadGate))), "Upload gate.");
        ReleaseGate releaseUpload{uploadGate.Get()};
        renderer.m_commandContext.GetCommandQueue()->Wait(uploadGate.Get(), 1);
        submit();
        CheckAdmission(renderer.GetPreparedTextureUploadState(uploadTicket) == PreparedTextureUpload::State::Submitted,
            "Upload did not become submitted.");
        preparation.Cancel(ticket);
        renderer.PollPreparedTextureUploads();
        CheckAdmission(!renderer.m_materialAdmission && renderer.GetPreparedTextureUploadState(uploadTicket) ==
            PreparedTextureUpload::State::Submitted && renderer.m_materialPublication.desired.size() == countBeforeCancel,
            "Cancellation released in-flight upload or published material.");
        uploadGate->Signal(1);
        CheckAdmission(renderer.WaitForGPU(), "Canceled upload completion.");
        renderer.PollPreparedTextureUploads();
        CheckAdmission(!renderer.GetPreparedTextureUploadState(uploadTicket) && advance(canceled).status == Status::Stale,
            "Canceled upload survived its fence or stale candidate accepted.");
        canceled.reset();
        canceled = prepare(canceledID);
        CheckAdmission(advance(canceled).status == Status::Pending, "Explicit cancellation fixture.");
        renderer.CancelMaterialAdmission(); renderer.PollPreparedTextureUploads();
        CheckAdmission(!renderer.m_materialAdmission && !catalog.FindPublishedMaterial(canceledID.ToReference(), renderer),
            "Explicit admission cancellation published material.");
        CheckAdmission(advance(canceled).status == Status::Pending, "Registry invalidation fixture.");
        CheckAdmission(registry.Unregister(canceledTexture), "Registry cancellation fixture.");
        renderer.PollPreparedTextureUploads();
        CheckAdmission(!renderer.m_materialAdmission && !catalog.FindPublishedMaterial(canceledID.ToReference(), renderer),
            "Registry invalidation depended on another admission call.");
        preparation.Cancel(ticket); canceled.reset();
        CheckAdmission(registry.Register({canceledTexture, AssetType::Texture,
            std::filesystem::path("Textures") / (canceledTexture.ToString() + ".tga")}), "Restore cancellation fixture.");
        canceled = prepare(canceledID);
        CheckAdmission(advance(canceled).status == Status::Pending, "Renderer invalidation fixture.");
        const auto lifetime = renderer.m_sceneResourceLifetime;
        ++renderer.m_sceneResourceLifetime;
        renderer.PollPreparedTextureUploads();
        renderer.m_sceneResourceLifetime = lifetime;
        CheckAdmission(!renderer.m_materialAdmission && advance(canceled).status == Status::Stale,
            "Renderer lifetime invalidation retained or silently restarted admission.");
        preparation.Cancel(ticket); canceled.reset();
        Log::Info("Material admission passed: unused plain/textured/multiple materials, shared textures, stable append indices, old/new GPU table readback, in-flight slot retirement, descriptor/upload pressure, repeated admission, stale material/texture/session rejection and checked binding publication; unchanged object/AS membership.");
    }

    static void Shutdown(Renderer& renderer, Engine& engine, FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime,
        const ProjectPaths& paths, ProjectAssetRegistry& registry)
    {
        ProjectMaterialPreparationService preparation(engine.GetTexturePreparationService());
        const auto material = *AssetID::Generate(), texture = *AssetID::Generate();
        const auto textureRelative = std::filesystem::path("Textures") / (texture.ToString() + ".tga");
        std::filesystem::copy_file(paths.GetRoot() / "Textures/streamed.tga", paths.GetRoot() / textureRelative);
        MaterialAssetData data; data.baseColorTexture = texture.ToReference();
        const auto materialRelative = std::filesystem::path("Materials") / (material.ToString() + ".material");
        std::string error;
        CheckAdmission(MaterialAssetSerializer::SaveToFile(data, paths.GetRoot() / materialRelative, &error) &&
            registry.Register({material, AssetType::Material, materialRelative}) &&
            registry.Register({texture, AssetType::Texture, textureRelative}), error);
        std::uint64_t ticket = 0;
        CheckAdmission(preparation.Request(paths, registry, material, runtime.GetRuntimeIdentity(), ticket, error) ==
            ProjectMaterialPreparationService::Status::Pending, error);
        std::shared_ptr<const Candidate> candidate;
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while(Clock::now() < deadline)
        {
            auto result = preparation.Poll(ticket, paths, registry, runtime.GetRuntimeIdentity());
            if(result.status == ProjectMaterialPreparationService::Status::Ready) { candidate = result.candidate; break; }
            CheckAdmission(result.status == ProjectMaterialPreparationService::Status::Pending, result.message);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CheckAdmission(candidate && renderer.AdvanceMaterialAdmission(preparation, candidate, catalog, paths, registry, runtime).status ==
            Status::Pending && renderer.m_materialAdmission, "Shutdown admission fixture.");
        CheckAdmission(renderer.ReleaseSceneResources() && !renderer.m_materialAdmission && renderer.m_preparedTextureUploads.empty(),
            "Checked scene shutdown retained admission/upload ownership.");
        CheckAdmission(renderer.AdvanceMaterialAdmission(preparation, candidate, catalog, paths, registry, runtime).status == Status::Stale,
            "Released renderer accepted candidate.");
        Log::Info("Material admission lifecycle passed: submitted cancellation/fence retirement, automatic source/registry/renderer invalidation and checked shutdown.");
    }
};

void VerifyMaterialAdmission(Renderer& renderer, Engine& engine, FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime,
    const ProjectPaths& paths, ProjectAssetRegistry& registry)
{ MaterialAdmissionVerification::Run(renderer, engine, catalog, runtime, paths, registry); }
void VerifyMaterialAdmissionShutdown(Renderer& renderer, Engine& engine, FileSceneAssetCatalog& catalog, FileSceneRuntime& runtime,
    const ProjectPaths& paths, ProjectAssetRegistry& registry)
{ MaterialAdmissionVerification::Shutdown(renderer, engine, catalog, runtime, paths, registry); }
#endif
