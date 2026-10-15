#include "Engine/Engine.h"
#include "Engine/ArchiveSurfaceAdmission.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/CookedScene.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Core/Log.h"
#include <thread>
#include <stdexcept>
namespace { void RequireScene(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); } }
void Engine::PopulateArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene> source)
{
    const auto checkCancellation=[this]()
    {
        if(m_startup.mode==EngineStartup::Mode::PackagedRuntime)
        {
            m_window.ProcessMessages();
            if(m_window.ShouldClose()) throw std::runtime_error("Packaged scene activation canceled.");
        }
    };
    std::string error;
    auto* scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    RequireScene(scene && scene->IsArchiveActivation() && scene->ArchiveSource()==source && scene->GetScenePath().empty() && scene->GetProjectRoot().empty(),
        "Archive-only activation did not retain the exact source or fabricated a development path.");
    RequireScene(SceneDocumentsEqual(scene->m_runtime->GetFileScene().GetDocument(),source->metadata.document),"Activated scene document differs.");
    scene->m_runtime->m_cookedPublicationAuthorized=true;
    if(!m_materialPreparation) m_materialPreparation=std::make_unique<ProjectMaterialPreparationService>(m_texturePreparation);
    if(m_startup.mode==EngineStartup::Mode::PackagedRuntime) m_materialPreparation->RestrictToArchive();
    ArchiveSurfaceAdmission::Admit(m_renderer,*m_materialPreparation,scene->m_assets,*scene->m_runtime,checkCancellation);
    const auto wait=[&](std::uint64_t ticket)
    {
        auto result=m_meshPreparation.PollCooked(ticket);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(result.status==ProjectMeshPreparationService::Status::Pending && std::chrono::steady_clock::now()<deadline)
        { checkCancellation(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); result=m_meshPreparation.PollCooked(ticket); }
        RequireScene(result.status==ProjectMeshPreparationService::Status::Available,"Archive scene mesh worker failed: "+result.error);
        return result;
    };
    size_t published=0,deferred=0;
    for(size_t index=0;index<source->metadata.document.objects.size();++index)
    {
        checkCancellation();
        const auto& authored=source->metadata.document.objects[index];
        auto selection=scene->GetObjectMetadata(index,&error);
        RequireScene(selection && selection->objectID==authored.id && selection->runtimeIndex==index && selection->materialReference==authored.materialReference &&
            selection->materialSlotBindings==authored.materialSlotBindings,"Stable object indices or authored material references differ.");
        if(authored.meshReference.empty()) continue;
        const auto asset=*AssetID::ParseReference(authored.meshReference);
        auto request=m_meshPreparation.RequestCooked({source->provider,asset,m_meshPreparation.Generation(),ProjectMeshPreparationService::PreparationMode::MetadataOnly},m_meshCPUCache);
        RequireScene(request.status==ProjectMeshPreparationService::Status::Accepted,request.error);
        auto metadata=wait(request.ticket);
        auto retained=scene->m_assets.RetainPreparedCookedMetadata(authored.meshReference,metadata,m_meshPreparation);
        RequireScene(retained.status==ProjectMeshPreparationService::Status::Available && !metadata.geometry,"Packaged metadata-only retention failed: "+retained.error);
        m_meshPreparation.Cancel(metadata.ticket);
        request=m_meshPreparation.RequestCooked({source->provider,asset,m_meshPreparation.Generation()},m_meshCPUCache);
        RequireScene(request.status==ProjectMeshPreparationService::Status::Accepted,request.error);
        auto full=wait(request.ticket);
        if(scene->GetScene().GetRenderObjects()[index].materialsDeferred)
        {
            RequireScene(!scene->GetScene().GetRenderObjects()[index].material && !scene->GetScene().GetRenderObjects()[index].mesh,
                "Deferred packaged materials were substituted or rendered.");
            ++deferred; m_meshPreparation.Cancel(full.ticket); continue;
        }
        auto prepared=scene->m_assets.ResolvePreparedCookedMesh(authored.meshReference,full,m_meshPreparation,m_renderer,*scene->m_runtime);
        RequireScene(prepared.status==ProjectMeshPreparationService::Status::Available && prepared.candidate,"Archive catalog geometry preparation failed: "+prepared.error);
        const auto token=scene->m_runtime->PrepareMeshResidency(*selection,scene->m_assets,*prepared.candidate,&error);
        RequireScene(bool(token),"Packaged residency token rejected: "+error);
        RequireScene(PrepareMeshResidencyAcceptance(*token,error),"Existing residency coordinator rejected archive scene: "+error);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        MeshResidencyPublication::AcceptanceResult result;
        do
        {
            checkCancellation();
            result=m_meshResidencyPublication.PublishAtBoundary(m_renderer,*scene->m_runtime,scene->m_assets);
            if(result.published || result.status!=MeshResidencyPublication::Status::Retry) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while(std::chrono::steady_clock::now()<deadline);
        RequireScene(result.published,"Existing checked archive mesh publication failed: "+result.message);
        RequireScene(scene->GetScene().GetRenderObjects()[index].mesh && scene->m_runtime->GetObjectMapping().idToRuntimeIndex.at(authored.id)==index,
            "Publication changed stable object indices or failed to adopt geometry.");
        const auto& resident=scene->GetScene().GetRenderObjects()[index];
        RequireScene(resident.material==scene->m_assets.FindPublishedMaterial(authored.materialReference,m_renderer),"Published archive fallback differs from authored material identity.");
        for(const auto& slot:resident.mesh->GetMetadata().materialSlots)
        {
            const auto override=authored.materialSlotBindings.find(slot.id);
            const auto& reference=override==authored.materialSlotBindings.end()?authored.materialReference:override->second;
            RequireScene(resident.GetSlotMaterial(slot.id)==scene->m_assets.FindPublishedMaterial(reference,m_renderer),"Published archive slot differs from authored material identity.");
        }
        ++published;
        CancelMeshResidencyAcceptance(); // Cancels authorization after adoption, not catalog/GPU ownership.
    }
    Log::Info("Packaged scene ready: " + std::to_string(published) + " resident objects, " + std::to_string(deferred) + " deferred objects.");
}
