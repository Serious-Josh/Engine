#include "assets/CookedScene.h"
#include "Scene/SceneMaterialBindingPublication.h"

#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Renderer/Renderer.h"
#include "Renderer/ShaderData.h"

#include <algorithm>
#include <type_traits>

namespace
{
    constexpr std::size_t MaxBindingObjects = 16384;
    constexpr std::size_t MaxBindingMaterials = 262144;
    bool SameIdentity(const SceneObjectMetadata& a, const SceneObjectMetadata& b)
    {
        return a.sceneID == b.sceneID && a.objectID == b.objectID &&
            a.runtimeIdentity == b.runtimeIdentity && a.structureRevision == b.structureRevision &&
            a.bindingRevision == b.bindingRevision && a.meshReference == b.meshReference &&
            a.materialReference == b.materialReference && a.materialSlotBindings == b.materialSlotBindings;
    }
}

struct SceneMaterialBindingPublication::Prepared
{
    SceneObjectMetadata selection;
    FileScene original;
    FileScene candidate;
    SceneDocument liveBefore;
    Mesh* mesh = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS vertexAddress = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indexAddress = 0;
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    MeshMetadata layout;
    Material* fallback = nullptr;
    std::map<std::string, Material*> runtimeSlots;
    std::map<std::string, Material*> committedSlots;
    std::vector<RenderObject> bindings;
    std::vector<std::vector<unsigned int>> indices;
    std::vector<RayTracingInstanceData> records;
    StructuredBuffer instances;
    StructuredBuffer slots;
    std::unique_ptr<RayTracingSceneResources::RetiredMaterialBindings> retirement;
    std::uint64_t rendererLifetime = 0;
    std::uint64_t bindingEpoch = 0;
    const FileSceneAssetCatalog* catalog = nullptr;
    const ProjectAssetRegistry* registry = nullptr;
    std::filesystem::path project;
    const void* membership = nullptr;
    std::size_t materialCount = 0;
    bool noOp = false;
    bool deferred = false;
    std::shared_ptr<const CookedScene::ArchiveScene> archiveSource;

    ~Prepared() { instances.Shutdown(); slots.Shutdown(); }
};

SceneMaterialBindingPublication::SceneMaterialBindingPublication() : m_thread(std::this_thread::get_id()) {}
SceneMaterialBindingPublication::~SceneMaterialBindingPublication() = default;
void SceneMaterialBindingPublication::Cancel() noexcept { m_prepared.reset(); }

SceneMaterialBindingPublication::Result SceneMaterialBindingPublication::PrepareDeferred(Renderer& renderer,const FileSceneRuntime& runtime,
    FileSceneAssetCatalog& catalog,const SceneObjectMetadata& expected)
{
    Cancel();
    try
    {
        if(!catalog.IsArchiveActivation()) return {Status::Unsupported,"Deferred admission requires an archive catalog."};
        auto current=runtime.GetObjectMetadata(expected.objectID);
        if(!current || !SameIdentity(*current,expected)) return {Status::Stale,"Deferred target is stale."};
        auto stage=std::make_unique<Prepared>(); stage->deferred=true; stage->archiveSource=catalog.ArchiveSource();
        stage->selection=*current; stage->catalog=&catalog; stage->rendererLifetime=renderer.m_sceneResourceLifetime;
        stage->fallback=catalog.FindPublishedMaterial(current->materialReference,renderer);
        if(!stage->fallback) return {Status::Retry,"Authored fallback material is not admitted."};
        for(const auto& [slot,ref]:current->materialSlotBindings)
        {
            auto* material=catalog.FindPublishedMaterial(ref,renderer);
            if(!material) return {Status::Retry,"Authored slot material is not admitted."};
            stage->runtimeSlots.emplace(slot,material);
        }
        stage->committedSlots=stage->runtimeSlots; m_prepared=std::move(stage);
        auto result=Validate(renderer,runtime,catalog); if(result.status!=Status::Ready) Cancel(); return result;
    }
    catch(const std::bad_alloc&) { Cancel(); return {Status::Retry,"Deferred binding allocation unavailable."}; }
    catch(const std::exception& e) { Cancel(); return {Status::Failed,e.what()}; }
}

SceneMaterialBindingPublication::Result SceneMaterialBindingPublication::PrepareAllSlots(
    Renderer& renderer, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
    const SceneObjectMetadata& expected, const AssetID& material)
{
    if(std::this_thread::get_id() != m_thread)
        return {Status::Stale, "Material publication requires its Engine thread."};
    Cancel();
    try
    {
        const auto current = runtime.GetObjectMetadata(expected.objectID);
        if(!current || !SameIdentity(*current, expected))
            return {Status::Stale, "All-slot target identity changed."};
        const auto* mesh = runtime.GetScene().GetRenderObjects()[current->runtimeIndex].mesh;
        if(!mesh)
            return {Status::Unsupported, "All-slot assignment requires resident slot metadata."};
        std::map<std::string, std::string> overrides;
        const auto reference = material.ToReference();
        for(const auto& slot : mesh->GetMetadata().materialSlots)
            overrides.emplace(slot.id, reference);
        return Prepare(renderer, runtime, catalog, expected, overrides);
    }
    catch(const std::bad_alloc&) { return {Status::Retry, "All-slot preparation allocation failed."}; }
    catch(const std::exception& exception) { return {Status::Failed, exception.what()}; }
}

SceneMaterialBindingPublication::Result SceneMaterialBindingPublication::Validate(
    Renderer& renderer, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog) const
{
    using enum Status;
    if(std::this_thread::get_id() != m_thread || !m_prepared)
        return {Stale, "No preparation on this Engine thread."};
    const auto& prepared = *m_prepared;
    if(prepared.deferred)
    {
        const auto current=runtime.GetObjectMetadata(prepared.selection.objectID);
        if(!current || !SameIdentity(*current,prepared.selection) || catalog.ArchiveSource()!=prepared.archiveSource ||
            prepared.catalog!=&catalog || prepared.rendererLifetime!=renderer.m_sceneResourceLifetime || renderer.m_sceneResourceScene!=&runtime.GetScene())
            return {Stale,"Deferred binding scene/archive/renderer identity changed."};
        if(!renderer.CanEditPendingMaterialBindings(runtime.GetScene()) || runtime.HasPreparedScene() || runtime.HasMaterialAssignment() ||
            runtime.m_meshResidency || renderer.m_rayTracingScene.m_candidate) return {Retry,"Deferred binding boundary unavailable."};
        const auto& object=runtime.GetScene().GetRenderObjects()[current->runtimeIndex];
        if(object.mesh || !object.materialsDeferred || object.material || current->bindingRevision==UINT64_MAX)
            return {Stale,"Deferred object readiness changed."};
        std::string error;
        if(!catalog.ValidateRetainedResidencyMaterial(current->materialReference,prepared.fallback,&error) || !renderer.HasPublishedMaterial(*prepared.fallback))
            return {Stale,"Deferred fallback ownership changed: "+error};
        for(const auto& [slot,ref]:current->materialSlotBindings)
        {
            const auto found=prepared.runtimeSlots.find(slot);
            if(found==prepared.runtimeSlots.end() || !found->second || !renderer.HasPublishedMaterial(*found->second) ||
                !catalog.ValidateRetainedResidencyMaterial(ref,found->second,&error)) return {Stale,"Deferred slot admission changed: "+error};
        }
        return {Ready,{}};
    }
    auto& resources = renderer.m_rayTracingScene;
    const auto& membership = *resources.m_membership;
    std::error_code projectError;
    const auto project = std::filesystem::canonical(catalog.m_projectRoot, projectError);
    if(prepared.rendererLifetime != renderer.m_sceneResourceLifetime ||
        renderer.m_sceneResourceScene != &runtime.GetScene() || prepared.catalog != &catalog ||
        projectError || prepared.project != project || prepared.registry != catalog.m_registry ||
        prepared.membership != resources.m_membership.get() ||
        prepared.bindingEpoch != resources.m_materialBindingEpoch)
        return {Stale, "Scene, catalog, renderer lifetime or binding epoch changed."};
    if(!renderer.CanEditPendingMaterialBindings(runtime.GetScene()) || runtime.HasPreparedScene() ||
        runtime.HasMaterialAssignment() || runtime.m_meshResidency || resources.m_candidate ||
        membership.recordingGeneration || membership.submissionUncertain)
        return {Retry, "Another scene/renderer publication or recording is active."};
    if(prepared.materialCount != renderer.m_materialPublication.desired.size() ||
        resources.m_materialBindingEpoch == UINT64_MAX)
        return {Stale, "Material-table layout changed or binding identities exhausted."};

    std::string error;
    const auto current = runtime.GetObjectMetadata(prepared.selection.objectID, &error);
    if(!current || !SameIdentity(*current, prepared.selection))
        return {Stale, "Object, scene structure, mesh residency or authored bindings changed."};
    const auto& object = runtime.GetScene().GetRenderObjects()[current->runtimeIndex];
    if(object.mesh != prepared.mesh || object.material != prepared.fallback || !object.mesh ||
        object.mesh->GetVertexBufferAddress() != prepared.vertexAddress ||
        object.mesh->GetIndexBufferAddress() != prepared.indexAddress ||
        object.mesh->GetVertexCount() != prepared.vertexCount || object.mesh->GetIndexCount() != prepared.indexCount ||
        object.mesh->GetMetadata().materialSlots != prepared.layout.materialSlots ||
        object.mesh->GetMetadata().submeshes != prepared.layout.submeshes ||
        !resources.ValidateMaterialBindings(runtime.GetScene()))
        return {Stale, "Resident mesh layout or active renderer bindings changed."};
    if(runtime.m_objectMapping.bindingRevisions[current->runtimeIndex] == UINT64_MAX)
        return {Stale, "Object binding identities exhausted."};
    SceneDocument live;
    if(runtime.m_fileScene.GetPath() != prepared.original.GetPath() ||
        runtime.m_fileScene.IsDirty() != prepared.original.IsDirty() ||
        !SceneDocumentsEqual(runtime.m_fileScene.GetDocument(), prepared.original.GetDocument()) ||
        !CaptureSceneDocument(runtime.m_fileScene.GetDocument(), runtime.GetScene(), runtime.m_objectMapping, live, &error) ||
        !SceneDocumentsEqual(live, prepared.liveBefore))
        return {Stale, "Scene/document changed after material-binding preparation."};

    if(!catalog.ValidateRetainedResidencyMaterial(current->materialReference, prepared.fallback, &error) ||
        !renderer.HasPublishedMaterial(*prepared.fallback))
        return {Stale, "Fallback material registration or renderer admission changed. " + error};
    const auto& authored = prepared.candidate.GetDocument().objects;
    const auto target = std::find_if(authored.begin(), authored.end(),
        [&](const auto& entry) { return entry.id == current->objectID; });
    if(target == authored.end())
        return {Stale, "Prepared object disappeared."};
    for(std::size_t slotIndex = 0; slotIndex < prepared.layout.materialSlots.size(); ++slotIndex)
    {
        const auto& slot = prepared.layout.materialSlots[slotIndex];
        const auto override = target->materialSlotBindings.find(slot.id);
        const auto& reference = override == target->materialSlotBindings.end() ?
            target->materialReference : override->second;
        const auto material = prepared.runtimeSlots.at(slot.id);
        if(!catalog.ValidateRetainedResidencyMaterial(reference, material, &error) ||
            !renderer.HasPublishedMaterial(*material) ||
            (!prepared.indices.empty() && prepared.indices[current->runtimeIndex][slotIndex] != material->gpuIndex))
            return {Stale, "Material registration, ownership or table index changed. " + error};
    }
    return {Ready, {}};
}

SceneMaterialBindingPublication::Result SceneMaterialBindingPublication::Prepare(
    Renderer& renderer, const FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog,
    const SceneObjectMetadata& expected, const std::map<std::string, std::string>& overrides)
{
    using enum Status;
    if(std::this_thread::get_id() != m_thread)
        return {Stale, "Material publication requires its Engine thread."};
    Cancel();
    try
    {
        if(!renderer.CanEditPendingMaterialBindings(runtime.GetScene()) || runtime.HasPreparedScene() ||
            runtime.HasMaterialAssignment() || runtime.m_meshResidency || renderer.m_rayTracingScene.m_candidate)
            return {Retry, "Material preparation requires an idle active-scene boundary."};
        std::string error;
        const auto current = runtime.GetObjectMetadata(expected.objectID, &error);
        if(!current || !SameIdentity(*current, expected))
            return {Stale, "Material preparation target is stale."};
        const auto& objects = runtime.GetScene().GetRenderObjects();
        const auto& object = objects[current->runtimeIndex];
        if(!object.mesh || !object.material)
            return {Unsupported, "Incremental GPU bindings require a resident mesh; pending authored overrides are unchanged."};
        if(objects.size() > MaxBindingObjects || !ValidateMeshMetadata(object.mesh->GetMetadata(),
            object.mesh->IsIndexed() ? object.mesh->GetIndexCount() : object.mesh->GetVertexCount(), error))
            return {Unsupported, "Invalid mesh-slot layout or publication work limit. " + error};
        auto prepared = std::make_unique<Prepared>();
        prepared->selection = *current;
        prepared->mesh = object.mesh;
        prepared->vertexAddress = object.mesh->GetVertexBufferAddress();
        prepared->indexAddress = object.mesh->GetIndexBufferAddress();
        prepared->vertexCount = object.mesh->GetVertexCount();
        prepared->indexCount = object.mesh->GetIndexCount();
        prepared->layout = object.mesh->GetMetadata();
        prepared->fallback = object.material;
        for(const auto& [id, reference] : overrides)
        {
            if(!AssetID::ParseReference(reference) || std::none_of(prepared->layout.materialSlots.begin(),
                prepared->layout.materialSlots.end(), [&](const auto& slot) { return slot.id == id; }))
                return {Unsupported, "Overrides require existing stable slot IDs and canonical material AssetIDs."};
        }
        for(const auto& slot : prepared->layout.materialSlots)
        {
            const auto found = overrides.find(slot.id);
            Material* material = prepared->fallback;
            if(found != overrides.end())
            {
                // Preserve a retained version when its authored ID is unchanged.
                const auto old = current->materialSlotBindings.find(slot.id);
                material = old != current->materialSlotBindings.end() && old->second == found->second ?
                    object.GetSlotMaterial(slot.id) : catalog.FindPublishedMaterial(found->second, renderer);
            }
            if(!material || !renderer.HasPublishedMaterial(*material))
                return {Unsupported, "Material is not admitted to the active fixed renderer table; no synchronous loading or table growth is performed."};
            prepared->runtimeSlots.emplace(slot.id, material);
        }
        prepared->committedSlots = prepared->runtimeSlots;
        prepared->original = runtime.m_fileScene;
        prepared->candidate = runtime.m_fileScene;
        if(!CaptureSceneDocument(runtime.m_fileScene.GetDocument(), runtime.GetScene(), runtime.m_objectMapping,
            prepared->liveBefore, &error))
            return {Stale, std::move(error)};
        prepared->noOp = current->materialSlotBindings == overrides;
        if(!prepared->noOp)
        {
            auto document = prepared->liveBefore;
            const auto target = std::find_if(document.objects.begin(), document.objects.end(),
                [&](const auto& entry) { return entry.id == current->objectID; });
            if(target == document.objects.end())
                return {Stale, "Object disappeared from captured scene."};
            target->materialSlotBindings = overrides;
            if(!ValidateSceneDocument(document, &error))
                return {Unsupported, std::move(error)};
            prepared->candidate.EditDocument() = std::move(document);
        }
        auto& resources = renderer.m_rayTracingScene;
        prepared->rendererLifetime = renderer.m_sceneResourceLifetime;
        prepared->bindingEpoch = resources.m_materialBindingEpoch;
        prepared->membership = resources.m_membership.get();
        prepared->catalog = &catalog;
        prepared->registry = catalog.m_registry;
        prepared->project = std::filesystem::canonical(catalog.m_projectRoot);
        prepared->materialCount = renderer.m_materialPublication.desired.size();
        m_prepared = std::move(prepared);
        auto validation = Validate(renderer, runtime, catalog);
        if(validation.status != Ready) { Cancel(); return validation; }
        if(m_prepared->noOp)
            return {Ready, "Bindings are unchanged."};

        renderer.m_rayTracingScene.CollectRetiredMemberships(renderer.m_commandContext);
        if(renderer.m_resourceDescriptorHeap.GetAvailableCount() < 2)
        { Cancel(); return {Retry, "Two lookup descriptors are unavailable."}; }
        auto& stage = *m_prepared;
        const auto& membership = *resources.m_membership;
        if(membership.instanceRecords.size() != objects.size() || membership.slotMaterialIndices.size() != objects.size())
        { Cancel(); return {Unsupported, "Active lookup snapshot is incomplete."}; }
        std::size_t materialCount = 0;
        for(const auto& indices : membership.slotMaterialIndices)
        {
            if(indices.size() > MaxBindingMaterials - materialCount)
            { Cancel(); return {Unsupported, "Material lookup work limit exceeded."}; }
            materialCount += indices.size();
        }
        stage.records = membership.instanceRecords;
        stage.indices = membership.slotMaterialIndices;
        stage.bindings = membership.materialBindings;
        stage.bindings[current->runtimeIndex].slotMaterials = stage.runtimeSlots;
        auto& targetIndices = stage.indices[current->runtimeIndex];
        if(targetIndices.size() != stage.layout.materialSlots.size())
        { Cancel(); return {Stale, "Active target slot count changed."}; }
        for(std::size_t slot = 0; slot < targetIndices.size(); ++slot)
            targetIndices[slot] = stage.runtimeSlots.at(stage.layout.materialSlots[slot].id)->gpuIndex;
        std::vector<unsigned int> materials;
        materials.reserve(materialCount);
        for(std::size_t objectIndex = 0; objectIndex < stage.indices.size(); ++objectIndex)
        {
            const auto& indices = stage.indices[objectIndex];
            if(indices.size() > MaxBindingMaterials - materials.size())
            { Cancel(); return {Unsupported, "Material lookup work limit exceeded."}; }
            if(!indices.empty())
            {
                if(stage.records[objectIndex].slotMaterialOffset != materials.size())
                { Cancel(); return {Unsupported, "Active slot lookup offsets are incompatible."}; }
                stage.records[objectIndex].materialIndex = indices.front();
            }
            materials.insert(materials.end(), indices.begin(), indices.end());
        }
        if(!stage.slots.InitializeReadOnlyUpload(renderer.GetDevice(), materials.data(), sizeof(unsigned int),
            static_cast<UINT>(materials.size()), renderer.m_resourceDescriptorHeap))
        { Cancel(); return {Retry, "Material lookup allocation failed."}; }
        for(auto& instance : stage.records)
        {
            instance.slotMaterialBufferIndex = stage.slots.GetDescriptorIndex();
        }
        if(!stage.instances.InitializeReadOnlyUpload(renderer.GetDevice(), stage.records.data(), sizeof(RayTracingInstanceData),
            static_cast<UINT>(stage.records.size()), renderer.m_resourceDescriptorHeap))
        { Cancel(); return {Retry, "Instance lookup allocation failed."}; }
        stage.retirement = std::make_unique<RayTracingSceneResources::RetiredMaterialBindings>();
        return {Ready, {}};
    }
    catch(const std::bad_alloc&)
    { Cancel(); return {Retry, "Material publication storage allocation failed."}; }
    catch(const std::exception& exception)
    { Cancel(); return {Failed, exception.what()}; }
}

SceneMaterialBindingPublication::Result SceneMaterialBindingPublication::PublishAtBoundary(
    Renderer& renderer, FileSceneRuntime& runtime, FileSceneAssetCatalog& catalog)
{
    using enum Status;
    try
    {
        const auto validation = Validate(renderer, runtime, catalog);
        if(validation.status != Ready) return validation;
        auto& stage = *m_prepared;
        if(stage.deferred)
        {
            const auto index=runtime.m_objectMapping.idToRuntimeIndex.at(stage.selection.objectID);
            auto& object=runtime.m_scene.GetRenderObjects()[index]; auto& committed=runtime.m_objectMapping.committedObjects[index];
            object.material=stage.fallback; committed.material=stage.fallback;
            object.slotMaterials.swap(stage.runtimeSlots); committed.slotMaterials.swap(stage.committedSlots);
            object.materialsDeferred=false; committed.materialsDeferred=false;
            ++runtime.m_objectMapping.bindingRevisions[index];
            runtime.RevalidatePhysicsPresentationBindings(renderer);
            Cancel(); return {Ready,{},true};
        }
        if(stage.noOp) { Cancel(); return {Ready, "Bindings are unchanged.", false}; }
        auto& resources = renderer.m_rayTracingScene;
        resources.CollectRetiredMemberships(renderer.m_commandContext);
        const auto retired = std::find_if(resources.m_retiredMaterialBindings.begin(),
            resources.m_retiredMaterialBindings.end(), [](const auto& value) { return !value; });
        if(retired == resources.m_retiredMaterialBindings.end())
            return {Retry, "Material lookup retirement capacity is full."};
        const auto& membership = *resources.m_membership;
        if(membership.lastUseFence && membership.fenceLifetime != renderer.m_commandContext.GetSubmissionLifetime())
            return {Stale, "Active lookup fence belongs to another command-context lifetime."};
        if(renderer.m_commandContext.GetCompletedFenceValue() == UINT64_MAX)
            return {Failed, "Device loss prevents safe binding publication."};
        // Re-resolve the stable ID at acceptance. All fallible validation and
        // allocations precede this point; the following ownership swaps cannot fail.
        const auto index = runtime.m_objectMapping.idToRuntimeIndex.at(stage.selection.objectID);
        stage.retirement->fence = membership.lastUseFence;
        stage.retirement->lifetime = renderer.m_commandContext.GetSubmissionLifetime();
        static_assert(std::is_nothrow_move_assignable_v<FileScene>);
        runtime.m_fileScene = std::move(stage.candidate);
        runtime.m_scene.GetRenderObjects()[index].slotMaterials.swap(stage.runtimeSlots);
        runtime.m_objectMapping.committedObjects[index].slotMaterials.swap(stage.committedSlots);
        ++runtime.m_objectMapping.bindingRevisions[index];
        resources.m_membership->instanceDataBuffer.Swap(stage.retirement->instances);
        resources.m_membership->slotMaterials.Swap(stage.retirement->slots);
        resources.m_membership->instanceDataBuffer.Swap(stage.instances);
        resources.m_membership->slotMaterials.Swap(stage.slots);
        resources.m_membership->materialBindings.swap(stage.bindings);
        resources.m_membership->slotMaterialIndices.swap(stage.indices);
        resources.m_membership->instanceRecords.swap(stage.records);
        ++resources.m_materialBindingEpoch;
        runtime.RevalidatePhysicsPresentationBindings(renderer);
        *retired = std::move(stage.retirement);
        Cancel();
        return {Ready, {}, true};
    }
    catch(const std::bad_alloc&) { return {Retry, "Acceptance validation allocation failed."}; }
    catch(const std::exception& exception) { return {Failed, exception.what()}; }
}
