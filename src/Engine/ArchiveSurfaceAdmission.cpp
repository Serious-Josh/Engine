#include "Engine/ArchiveSurfaceAdmission.h"
#include "Renderer/Renderer.h"
#include "Scene/FileSceneAssetCatalog.h"
#include "Scene/FileSceneRuntime.h"
#include "Scene/SceneMaterialBindingPublication.h"
#include "assets/CookedScene.h"
#include <chrono>
#include <set>
#include <thread>
#include <stdexcept>
#include <cstring>

void ArchiveSurfaceAdmission::Admit(Renderer& renderer,ProjectMaterialPreparationService& preparation,FileSceneAssetCatalog& catalog,FileSceneRuntime& runtime, const std::function<void()>& checkCancellation)
{
    const auto source=catalog.ArchiveSource(); if(!source) throw std::runtime_error("Missing archive surface source.");
    const auto check=[](bool ok,const std::string& error){ if(!ok) throw std::runtime_error(error); };
    std::set<std::string> materials;
    for(const auto& o:source->metadata.document.objects)
    {
        if(auto id=AssetID::ParseReference(o.materialReference)) materials.insert(id->ToString());
        for(const auto& [slot,ref]:o.materialSlotBindings) materials.insert(AssetID::ParseReference(ref)->ToString());
    }
    unsigned frame=0;
    const auto submit=[&]
    {
        const auto slot=frame++%CommandContext::FrameCount;
        check(renderer.BeginTrackedRecording(slot),"Archive admission cannot begin tracked frame.");
        const auto generation=renderer.m_activeRecordingGeneration,revision=renderer.m_materialPublication.revision;
        auto* commands=renderer.m_commandContext.GetCommandList();
        renderer.RecordPreparedTextureUploads(commands);
        check(renderer.RecordMaterialTable(slot,commands) && renderer.EndTrackedRecording() && renderer.ExecuteTrackedRecording(slot),"Archive material frame submission failed.");
        const auto fence=renderer.m_commandContext.GetLastSubmissionFenceValue();
        for(auto& upload:renderer.m_preparedTextureUploads) upload.upload->ConfirmSubmission(generation,fence);
        check(renderer.m_materialDependencies.Confirm(slot,fence),"Archive texture dependency submission failed.");
        renderer.m_materialPublication.ConfirmSubmission(slot,revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); renderer.PollPreparedTextureUploads();
    };
    const auto original=renderer.m_materialPublication.desired;
    const auto lifetime=renderer.m_sceneResourceLifetime;
    for(const auto& text:materials)
    {
        if(checkCancellation) checkCancellation();
        const auto id=*AssetID::Parse(text); std::uint64_t ticket=0; std::string error;
        check(preparation.RequestArchive(source->provider,id,runtime.GetRuntimeIdentity(),ticket,error)==ProjectMaterialPreparationService::Status::Pending,error);
        struct Cancel { ProjectMaterialPreparationService& service; std::uint64_t ticket; ~Cancel(){service.Cancel(ticket);} } cancel{preparation,ticket};
        std::shared_ptr<const ProjectMaterialPreparationService::Candidate> candidate;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!candidate && std::chrono::steady_clock::now()<deadline)
        {
            if(checkCancellation) checkCancellation();
            auto result=preparation.PollArchive(ticket,runtime.GetRuntimeIdentity());
            check(result.status==ProjectMaterialPreparationService::Status::Pending || result.status==ProjectMaterialPreparationService::Status::Retry || result.status==ProjectMaterialPreparationService::Status::Ready,result.message);
            candidate=result.candidate; if(!candidate) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(bool(candidate),"Archive material preparation timed out.");
        MaterialAdmissionResult admitted;
        do
        {
            if(checkCancellation) checkCancellation();
            admitted=renderer.AdvanceArchiveMaterialAdmission(preparation,candidate,catalog,runtime);
            check(admitted.status==MaterialAdmissionStatus::Pending || admitted.status==MaterialAdmissionStatus::Retry || admitted.status==MaterialAdmissionStatus::Ready,admitted.message);
            if(admitted.status!=MaterialAdmissionStatus::Ready) submit();
        } while(admitted.status!=MaterialAdmissionStatus::Ready && std::chrono::steady_clock::now()<deadline);
        check(admitted.status==MaterialAdmissionStatus::Ready && admitted.material && renderer.HasPublishedMaterial(*admitted.material),"Archive material admission timed out.");
        check(admitted.material->model==candidate->parameters.model && admitted.material->roughness==candidate->parameters.roughness,
            "Archived material parameters differ from admitted material.");
        const Texture* actual[]={admitted.material->texture,admitted.material->normalTexture,admitted.material->roughnessTexture,
            admitted.material->metallicTexture,admitted.material->armTexture,admitted.material->aoTexture,admitted.material->heightTexture};
        for(size_t slot=0;slot<7;++slot) if(candidate->textures[slot])
        {
            check(actual[slot] && actual[slot]->GetResource(),"Archive texture resource missing after asynchronous upload.");
            const auto desc=actual[slot]->GetResource()->GetDesc(); const auto& expected=*candidate->textures[slot]->prepared;
            check(desc.Width==expected.width && desc.Height==expected.height && desc.MipLevels==expected.subresources.size() && desc.Format==expected.resourceFormat,
                "Archived texture dimensions/mips/format differ after upload.");
        }
        // Frame slots adopt appended tables using their existing safe-reuse protocol.
        for(size_t slot=0;slot<CommandContext::FrameCount;++slot) submit();
        check(renderer.m_materialPublication.desired.size()>=original.size() &&
            std::memcmp(original.data(),renderer.m_materialPublication.desired.data(),original.size()*sizeof(GPUMaterial))==0,"Existing GPU material indices/data changed.");
    }
    check(renderer.m_sceneResourceLifetime==lifetime,"Material admission rebuilt scene resources.");
    // Shared prepared dependencies must resolve to one catalog GPU texture.
    std::map<std::pair<std::string,ProjectTextureInterpretation>,const Texture*> uniqueTextures;
    for(const auto& text:materials)
    {
        if(checkCancellation) checkCancellation();
        auto* material=catalog.FindPublishedMaterial("asset:"+text,renderer); check(material!=nullptr,"Admitted archive material missing.");
        const auto sourceData=catalog.ArchiveSource()->provider;
        auto raw=sourceData->ReadEntry(*AssetID::Parse(text),EngineAssetPak::Type::Material,CookedSurface::Version,CookedSurface::MaterialLimit);
        const auto decoded=CookedSurface::DecodeMaterial(raw);
        const auto refs=CookedSurface::References(decoded.parameters);
        const Texture* slots[]={material->texture,material->normalTexture,material->roughnessTexture,material->metallicTexture,material->armTexture,material->aoTexture,material->heightTexture};
        for(size_t slot=0;slot<7;++slot) if(refs[slot])
        {
            check(slots[slot] && slots[slot]->GetResource(),"Archived texture did not reach GPU admission.");
            auto [found,inserted]=uniqueTextures.emplace(std::make_pair(*refs[slot],CookedSurface::Interpretation(slot)),slots[slot]);
            check(inserted || found->second==slots[slot],"Shared archive texture uploaded more than once.");
        }
    }
    SceneMaterialBindingPublication publication;
    for(size_t index=0;index<source->metadata.document.objects.size();++index)
    {
        if(!runtime.GetScene().GetRenderObjects()[index].materialsDeferred) continue;
        auto current=runtime.GetObjectMetadata(index); check(bool(current),"Archive deferred object lost identity.");
        auto prepared=publication.PrepareDeferred(renderer,runtime,catalog,*current);
        check(prepared.status==SceneMaterialBindingPublication::Status::Ready,prepared.message);
        auto bound=publication.PublishAtBoundary(renderer,runtime,catalog);
        check(bound.published && !runtime.GetScene().GetRenderObjects()[index].materialsDeferred,"Authored archive materials were not atomically bound: "+bound.message);
    }
}
