#include "SceneMembershipPublication.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Renderer/Renderer.h"
#include <algorithm>
#include <new>
#include <stdexcept>

struct SceneMembershipPublication::Prepared
{
    const FileSceneRuntime* source=nullptr;
    const FileSceneAssetCatalog* catalog=nullptr;
    std::uint64_t runtime=0, structure=0, residency=0, renderer=0, bindings=0;
    // Edge/order changes keep the structure revision; the hierarchy revision detects them.
    std::uint64_t hierarchy=0;
    SceneDocument document;
    std::vector<RenderObject> objects;
    std::vector<std::uint64_t> bindingRevisions;
    std::unique_ptr<FileSceneRuntime> target;
    RayTracingSceneResources::ReplacementCandidate gpu;
};
SceneMembershipPublication::SceneMembershipPublication():m_thread(std::this_thread::get_id()) {}
SceneMembershipPublication::~SceneMembershipPublication()=default;
std::optional<SceneObjectMetadata> SceneMembershipPublication::GetPreparedSelection(std::string_view id) const
{ return m_prepared ? m_prepared->target->GetObjectMetadata(id) : std::nullopt; }
void SceneMembershipPublication::Cancel() noexcept { m_prepared.reset(); }
SceneMembershipPublication::Result SceneMembershipPublication::PrepareInsertion(Renderer& r,const FileSceneRuntime& s,
    FileSceneAssetCatalog& c,const SceneObjectDocument& object,std::size_t index,unsigned int slot,std::string_view bindingSourceID)
{ return Prepare(r,s,c,&object,{},index,slot,bindingSourceID); }
SceneMembershipPublication::Result SceneMembershipPublication::PrepareRemoval(Renderer& r,const FileSceneRuntime& s,
    FileSceneAssetCatalog& c,std::string_view id,unsigned int slot)
{ return Prepare(r,s,c,nullptr,id,0,slot); }
SceneMembershipPublication::Result SceneMembershipPublication::PrepareDocument(Renderer& renderer,
    const FileSceneRuntime& source, FileSceneAssetCatalog& catalog, const SceneDocument& document, unsigned int slot, const std::map<std::string, std::string>& bindingSources)
{
    return Prepare(renderer, source, catalog, nullptr, {}, 0, slot, {}, &document, &bindingSources);
}
SceneMembershipPublication::Result SceneMembershipPublication::Prepare(Renderer& r,const FileSceneRuntime& source,
    FileSceneAssetCatalog& catalog,const SceneObjectDocument* insertion,std::string_view removal,std::size_t index,unsigned int slot,std::string_view bindingSourceID,const SceneDocument* document, const std::map<std::string, std::string>* bindingSources)
{
    if(std::this_thread::get_id()!=m_thread) return {Status::Failed,"Membership transactions require the owning main thread."};
    if(m_prepared) return {Status::Retry,"Cancel or publish the outstanding membership transaction first."};
    if(catalog.IsArchiveActivation()) return {Status::Unsupported,"Dynamic membership is editor file-scene infrastructure."};
    if(!r.m_sceneResourceLifetime || r.m_sceneResourceScene!=&source.GetScene()) return {Status::Stale,"Active renderer scene changed."};
    if(source.HasPreparedScene() || source.HasMaterialAssignment() || r.HasUnresolvedDisplacementUpdate() ||
        r.m_commandRecordingGeneration || r.m_activeRecordingGeneration || r.m_sceneInitializationAttempt.candidate)
        return {Status::Retry,"Finish conflicting scene preparation or recording before preparing membership."};
    if(r.m_commandSubmissionAttempted || r.m_resizeFailed || r.m_geometryUpdateFailed || r.m_sceneInitializationAttempt.failed)
        return {Status::Failed,"Renderer requires checked recovery."};
    if(source.GetScene().GetRenderObjects().size()+(insertion?1:0)>RayTracingSceneResources::MaxCandidateObjects)
        return {Status::Unsupported,"Membership exceeds the bounded object limit."};
    try
    {
        auto p=std::make_unique<Prepared>();
        std::string error;
        if(!CaptureSceneDocument(source.GetFileScene().GetDocument(),source.GetScene(),source.GetObjectMapping(),p->document,&error))
            return {Status::Stale,error};
        if(insertion && !document)
        {
            if(index>p->document.objects.size() || source.GetObjectMapping().idToRuntimeIndex.contains(insertion->id))
                return {Status::Stale,"Insertion requires an unused stable object ID and valid index."};
            // Validate the inserted object in place, so its parent edge is checked against the
            // real graph rather than dangling in a single-object document.
            auto validation=p->document;
            validation.objects.insert(validation.objects.begin()+static_cast<std::ptrdiff_t>(index),*insertion);
            if(!ValidateSceneDocument(validation,&error)) return {Status::Unsupported,error};
            if(!catalog.ValidateMetadataMaterialReference(insertion->materialReference.empty()?"builtin:default":insertion->materialReference,&error))
                return {Status::Unsupported,error};
            for(const auto& [slotID,ref]:insertion->materialSlotBindings)
                if(!catalog.ValidateMetadataMaterialReference(ref,&error)) return {Status::Unsupported,error};
            const auto admitted=[&](const std::string& ref) { return catalog.FindPublishedMaterial(ref.empty()?"builtin:default":ref,r)!=nullptr; };
            if(!admitted(insertion->materialReference)) return {Status::Retry,"Inserted fallback material admission is pending."};
            for(const auto& [slotID,ref]:insertion->materialSlotBindings)
                if(!admitted(ref)) return {Status::Retry,"Inserted slot material admission is pending."};
        }
        SceneDocument proposed=p->document;
        if(document) proposed = *document;
        else if(insertion) proposed.objects.insert(proposed.objects.begin()+index,*insertion);
        else
        {
            const auto found=std::find_if(proposed.objects.begin(),proposed.objects.end(),[&](const auto& o) { return o.id==removal; });
            if(found==proposed.objects.end()) return {Status::Stale,"Removal stable object ID is absent."};
            proposed.objects.erase(found);
        }
        if(!ValidateSceneDocument(proposed, &error) || proposed.id != p->document.id ||
            proposed.objects.size() > RayTracingSceneResources::MaxCandidateObjects)
        {
            return {Status::Unsupported, "Invalid hierarchy membership candidate: " + error};
        }

        for(const auto& authored : proposed.objects)
        {
            if(source.GetObjectMapping().idToRuntimeIndex.contains(authored.id) ||
                (bindingSources && bindingSources->contains(authored.id)))
            {
                continue;
            }

            if(!catalog.ValidateMetadataMeshReference(authored.meshReference, &error) ||
                !catalog.ValidateMetadataMaterialReference(authored.materialReference.empty() ? "builtin:default" : authored.materialReference, &error))
            {
                return {Status::Unsupported, error};
            }

            if(!catalog.FindPublishedMaterial(authored.materialReference.empty() ? "builtin:default" : authored.materialReference, r))
            {
                return {Status::Unsupported, "History asset is unavailable; admit the material before retrying."};
            }

            for(const auto& [materialSlot, reference] : authored.materialSlotBindings)
            {
                if(!catalog.ValidateMetadataMaterialReference(reference, &error) || !catalog.FindPublishedMaterial(reference, r))
                {
                    return {Status::Unsupported, "History slot material is unavailable: " + error};
                }
            }
        }

        PreparedScene prepared;
        prepared.environmentReference=proposed.environmentReference;
        prepared.camera=proposed.camera; prepared.lightingSettings=proposed.lightingSettings;
        prepared.objects.reserve(proposed.objects.size());
        for(const auto& authored:proposed.objects)
        {
            RenderObject object;
            const auto old=source.GetObjectMapping().idToRuntimeIndex.find(authored.id);
            if(old!=source.GetObjectMapping().idToRuntimeIndex.end()) object=source.GetScene().GetRenderObjects()[old->second];
            else
            {
                const auto sourceID = bindingSources && bindingSources->contains(authored.id) ?
                    std::string_view(bindingSources->at(authored.id)) : bindingSourceID;

                if(!sourceID.empty())
                {
                    const auto donor=source.GetObjectMapping().idToRuntimeIndex.find(std::string(sourceID));
                    const auto metadata=source.GetObjectMetadata(sourceID);
                    if(donor==source.GetObjectMapping().idToRuntimeIndex.end() || !metadata ||
                        metadata->meshReference!=authored.meshReference || metadata->materialReference!=authored.materialReference ||
                        metadata->materialSlotBindings!=authored.materialSlotBindings)
                        return {Status::Stale,"Duplicate source binding identity changed."};
                    object=source.GetScene().GetRenderObjects()[donor->second];
                    object.transform=authored.transform; object.previousTransform=authored.transform;
                    prepared.objects.push_back({authored.id,authored.name,std::move(object),authored.meshReference.empty()});
                    continue;
                }
                // Only catalog-approved, already-published borrows. Never call a
                // synchronous asset resolver, even for fresh Inspector versions.
                for(const auto& original:p->document.objects)
                    if(original.meshReference==authored.meshReference)
                    {
                        object.mesh=source.GetScene().GetRenderObjects()[source.GetObjectMapping().idToRuntimeIndex.at(original.id)].mesh;
                        if(object.mesh) break;
                    }
                if(!object.mesh)
                {
                    if(!catalog.ValidateMetadataMeshReference(authored.meshReference,&error)) return {Status::Unsupported,error};
                    object.mesh=catalog.FindRetainedMesh(authored.meshReference);
                }
                object.material=catalog.FindPublishedMaterial(authored.materialReference.empty()?"builtin:default":authored.materialReference,r);
                object.transform=authored.transform; object.previousTransform=authored.transform;
                if(object.mesh)
                    for(const auto& slot:object.mesh->GetMetadata().materialSlots) object.slotMaterials.emplace(slot.id,object.material);
                for(const auto& [slot,ref]:authored.materialSlotBindings)
                {
                    if(object.mesh && !object.slotMaterials.contains(slot)) return {Status::Unsupported,"Inserted material slot does not exist in mesh metadata."};
                    object.slotMaterials.insert_or_assign(slot,catalog.FindPublishedMaterial(ref,r));
                }
            }
            object.transform = authored.transform;
            prepared.objects.push_back({authored.id,authored.name,std::move(object),authored.meshReference.empty()});
        }
        p->target=std::make_unique<FileSceneRuntime>();
        p->target->m_fileScene=source.GetFileScene();
        p->target->m_fileScene.EditDocument()=std::move(proposed);

        // The target continues the source's hierarchy revision sequence and installs the
        // proposed forest in the same replacement as its new runtime slots.
        p->target->m_hierarchy=source.m_hierarchy;
        SceneHierarchyTopology topology;
        std::vector<std::string_view> slotIDs;
        slotIDs.reserve(prepared.objects.size());

        for(const auto& object : prepared.objects)
        {
            slotIDs.push_back(object.id);
        }

        if(!FileSceneRuntime::PrepareCommitHierarchy(p->target->m_hierarchy, p->target->m_fileScene.GetDocument().objects,
            slotIDs, topology, &error))
        {
            return {Status::Unsupported,error};
        }

        if(!CommitPreparedEditorScene(prepared,p->target->m_fileScene.GetDocument(),p->target->m_scene,p->target->m_objectMapping,&error,
            std::move(topology)))
            return {Status::Unsupported,error};
        p->target->GetScene().SetDisplacementPatchLODs(source.GetScene().GetDisplacementPatchLODs());
        // Published geometry can be shared only through the stable BLAS registry.
        // Retained but not registered geometry stays pending for normal residency.
        auto& targetObjects=p->target->GetScene().GetRenderObjects();
        for(std::size_t i=0;i<targetObjects.size();++i)
        {
            auto& object=targetObjects[i];
            const auto& authored=p->target->GetFileScene().GetDocument().objects[i];
            const auto survivor=source.GetObjectMapping().idToRuntimeIndex.find(authored.id);
            if(survivor!=source.GetObjectMapping().idToRuntimeIndex.end())
            {
                const auto& old=source.GetScene().GetRenderObjects()[survivor->second];
                if(old.mesh!=object.mesh || old.material!=object.material || old.slotMaterials!=object.slotMaterials)
                    return {Status::Unsupported,"Membership preparation changed a surviving asset binding."};
            }
            if(object.mesh && !r.m_rayTracingScene.m_blasRegistry.meshToBottomLevel.contains(object.mesh))
            {
                object.mesh=nullptr;
                p->target->m_objectMapping.committedObjects[i].mesh=nullptr;
            }
            if(object.mesh && (!r.m_selectionMaskPass.IsInitialized() || !r.m_selectionMaskPass.IsCompatible(object.mesh->GetVertexLayout())))
                return {Status::Unsupported,"Resident membership requires the existing compatible selection-mask pipeline."};
        }
        p->source=&source; p->catalog=&catalog; p->runtime=source.GetRuntimeIdentity();
        p->structure=source.GetScene().GetStructureRevision(); p->residency=source.GetResidencyRevision();
        p->hierarchy=source.GetHierarchyRevision();
        p->renderer=r.m_sceneResourceLifetime; p->bindings=r.m_rayTracingScene.m_materialBindingEpoch;
        p->objects=source.GetScene().GetRenderObjects();
        p->bindingRevisions=source.GetObjectMapping().bindingRevisions;
        auto failure=RayTracingSceneResources::CandidateFailure::None;
        auto gpu=r.PrepareDXRMembershipCandidate(*p->target,catalog,{},slot,error,&source,&failure);
        if(!gpu)
        {
            // Submission uncertainty dominates; exhaustion is never reported as a deferral.
            if(r.m_commandSubmissionAttempted) return {Status::Failed,error};
            if(failure==RayTracingSceneResources::CandidateFailure::DescriptorExhausted) return {Status::ResourceExhausted,error};
            return {Status::Retry,error};
        }
        p->gpu=std::move(*gpu); m_prepared=std::move(p);
        return {Status::Ready,"Membership prepared; await candidate fence before publication."};
    }
    catch(const std::bad_alloc&) { return {Status::Retry,"Membership allocation unavailable; active state unchanged."}; }
    catch(const std::exception& e) { return {Status::Failed,e.what()}; }
}
SceneMembershipPublication::Result SceneMembershipPublication::PublishAtBoundary(Renderer& r,
    std::unique_ptr<FileSceneRuntime>& active,FileSceneAssetCatalog& catalog,
    std::optional<std::string_view> selectionID,std::optional<std::size_t>& selectionIndex)
{
    if(std::this_thread::get_id()!=m_thread) return {Status::Failed,"Membership publication requires the owning main thread."};
    if(!m_prepared || !active) return {Status::Stale,"No active membership transaction."};
    auto& p=*m_prepared;
    if(p.source!=active.get() || p.catalog!=&catalog || p.runtime!=active->GetRuntimeIdentity() ||
        p.structure!=active->GetScene().GetStructureRevision() || p.residency!=active->GetResidencyRevision() ||
        p.hierarchy!=active->GetHierarchyRevision() || !active->ValidateHierarchyAssociation() ||
        p.renderer!=r.m_sceneResourceLifetime || r.m_sceneResourceScene!=&active->GetScene() ||
        p.bindings!=r.m_rayTracingScene.m_materialBindingEpoch || p.gpu.m_rendererLifetime!=p.renderer ||
        p.bindingRevisions!=active->GetObjectMapping().bindingRevisions)
        return {Status::Stale,"Membership scene, renderer, revision or binding epoch changed."};
    if(r.m_commandSubmissionAttempted || r.m_resizeFailed || r.m_geometryUpdateFailed || r.m_sceneInitializationAttempt.failed)
        return {Status::Failed,"Renderer requires checked recovery."};
    if(active->HasPreparedScene() || active->HasMaterialAssignment() || r.HasUnresolvedDisplacementUpdate() ||
        r.m_commandRecordingGeneration || r.m_activeRecordingGeneration || !r.m_rayTracingScene.CanRetireActiveMembership())
        return {Status::Retry,"Recording, conflicting preparation or retirement capacity is not ready."};
    if(r.m_rayTracingScene.m_membership->lastUseFence &&
        r.m_rayTracingScene.m_membership->fenceLifetime!=r.m_commandContext.GetSubmissionLifetime())
        return {Status::Failed,"Active membership fence provenance changed."};
    try
    {
        std::string error;
        SceneDocument current;
        if(!CaptureSceneDocument(active->GetFileScene().GetDocument(),active->GetScene(),active->GetObjectMapping(),current,&error))
            return {Status::Stale,error};
        // Camera/light motion does not alter membership. Copy their live state at commit.
        current.camera=p.document.camera; current.lightingSettings=p.document.lightingSettings;
        if(!SceneDocumentsEqual(current,p.document)) return {Status::Stale,"Authoritative object ordering, references or transforms changed."};
        const auto& objects=active->GetScene().GetRenderObjects();
        if(objects.size()!=p.objects.size()) return {Status::Stale,"Object count changed."};
        for(std::size_t i=0;i<objects.size();++i)
            if(objects[i].mesh!=p.objects[i].mesh || objects[i].material!=p.objects[i].material ||
                objects[i].slotMaterials!=p.objects[i].slotMaterials || objects[i].materialsDeferred!=p.objects[i].materialsDeferred)
                return {Status::Stale,"Live mesh/material binding generation changed."};
        const auto status=r.m_rayTracingScene.PollReplacementCandidate(p.gpu,r.m_commandContext,*p.target,catalog,error);
        if(status==RayTracingSceneResources::CandidateStatus::Pending) return {Status::Retry,"Await candidate GPU fence."};
        if(status==RayTracingSceneResources::CandidateStatus::Failed) return {Status::Failed,error};
        if(status!=RayTracingSceneResources::CandidateStatus::Ready) return {Status::Stale,error};
        RayTracingSceneResources::AdoptionEvidence evidence;
        if(r.m_rayTracingScene.PrepareRegistryAdoption(p.gpu,evidence,error)!=RayTracingSceneResources::AdoptionReadiness::Ready)
            return {Status::Stale,error};
        std::optional<std::size_t> mapped;
        if(selectionID)
        {
            const auto found=p.target->GetObjectMapping().idToRuntimeIndex.find(std::string(*selectionID));
            if(found!=p.target->GetObjectMapping().idToRuntimeIndex.end()) mapped=found->second;
        }
        p.target->GetScene().GetCamera()=active->GetScene().GetCamera();
        p.target->GetScene().SetLightingSettings(active->GetScene().GetLightingSettings());
        Result success{Status::Ready,"Membership published."};
        // All final validation has passed. This allocates only optional advisory evidence.
        r.m_rayTracingScene.PrepareProspectiveJoin(p.gpu, *p.target, catalog,
            r.m_commandContext, r.m_sceneResourceLifetime);
        auto geometryHandoff = r.m_rayTracingScene.PreparePublicationHandoff(p.gpu, *p.target, this,
            RayTracingSceneResources::PublicationKind::Membership);
        // Allocation-free exclusive commit: no frame observes mixed CPU/GPU order.
        r.m_rayTracingScene.AdoptPreparedCandidate(p.gpu,evidence);
        active.swap(p.target);
        r.m_sceneResourceScene=&active->GetScene();
        selectionIndex=mapped;
        r.InvalidateSceneLighting(SceneLightingInvalidation::Membership);
        r.ResetSceneHistory();
        r.m_rayTracingScene.CommitPublicationAcceptance(std::move(geometryHandoff), *active,
            r.m_sceneResourceScene, r.m_sceneResourceLifetime, this);
        m_prepared.reset();
        return success;
    }
    catch(const std::bad_alloc&) { return {Status::Retry,"Publication allocation unavailable; active state unchanged."}; }
    catch(const std::exception& e) { return {Status::Stale,e.what()}; }
}
