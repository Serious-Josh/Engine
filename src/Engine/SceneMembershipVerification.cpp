#include "Engine/Engine.h"
#ifndef NDEBUG
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/SceneMembershipPublication.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"
#include "imgui.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <chrono>
#include <algorithm>
#include <thread>
#include <stdexcept>
namespace
{
void CheckMembership(bool value,const std::string& error) { if(!value) throw std::runtime_error("Membership: "+error); }
using Status=SceneMembershipPublication::Status;
}
class SceneMembershipVerification
{
public:
static void Run(Engine& engine,FileBackedDemoScene& owner)
{
    auto& r=engine.m_renderer;
    auto& resources=r.m_rayTracingScene;
    auto& active=owner.m_runtime;
    auto& catalog=owner.m_assets;
    auto& context=r.m_commandContext;
    const auto drains=context.GetGPUDrainCount();
    CheckMembership(active->GetScene().GetRenderObjects().size()==2,"Fixture must have two resident objects.");
    const auto original=active->GetFileScene().GetDocument().objects;
    const auto* mesh=active->GetScene().GetRenderObjects()[0].mesh;
    CheckMembership(mesh && mesh==active->GetScene().GetRenderObjects()[1].mesh,"Fixture geometry is not shared.");
    const auto blasCount=resources.m_blasRegistry.bottomLevels.size();
    const auto* blas=resources.m_blasRegistry.bottomLevels.front().get();
    CheckMembership(blasCount==1,"Fixture needs one shared BLAS.");
    auto render=[&](std::optional<std::size_t> selection={}) {
        r.GetDebugUI().BeginFrame();
        RendererOptions options; if(selection) { options.selectedObject=*selection; options.selectedObjects={*selection}; }
        CheckMembership(r.Render(active->GetScene(),options)!=Renderer::FrameRenderResult::Error,"Raster/DXR frame failed.");
    };
    auto wait=[&](auto predicate) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!predicate()) { CheckMembership(std::chrono::steady_clock::now()<deadline,"Fence/transaction timed out."); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    };
    auto prepare=[&](auto operation) {
        wait([&] { const auto result=operation(); CheckMembership(result.status==Status::Ready || result.status==Status::Retry,result.message); return result.status==Status::Ready; });
    };
    auto publish=[&](SceneMembershipPublication& pub,std::optional<std::string_view> selection) {
        std::optional<std::size_t> mapped;
        wait([&] { const auto result=pub.PublishAtBoundary(r,active,catalog,selection,mapped);
            CheckMembership(result.status==Status::Ready || result.status==Status::Retry,result.message); return result.status==Status::Ready; });
        engine.m_renderedSceneStructureRevision=active->GetScene().GetStructureRevision();
        engine.m_editor.PublishRestoredObjectSelection(owner,mapped ? owner.GetObjectMetadata(*mapped) : std::nullopt,
            std::string(engine.m_demoSceneManager.GetActiveSceneName()));
        return mapped;
    };
    const auto check=[&](const std::vector<std::string>& order) {
        CheckMembership(active->GetFileScene().GetDocument().objects.size()==order.size(),"Document count differs.");
        CheckMembership(resources.m_membership->materialBindings.size()==order.size(),"GPU binding count differs.");
        for(std::size_t i=0;i<order.size();++i)
        {
            const auto metadata=active->GetObjectMetadata(order[i]);
            CheckMembership(metadata && metadata->runtimeIndex==i && active->GetFileScene().GetDocument().objects[i].id==order[i],"Stable ID/index/order mismatch.");
            const auto& object=active->GetScene().GetRenderObjects()[i];
            if(!object.mesh) continue;
            const auto instance=resources.m_membership->renderObjectToInstance[i];
            CheckMembership(instance<resources.m_membership->instances.size() && resources.m_membership->instances[instance].instanceID==i,"DXR/picking index mapping mismatch.");
            CheckMembership(resources.m_membership->materialBindings[i].mesh==object.mesh &&
                resources.m_membership->materialBindings[i].slotMaterials==object.slotMaterials,"Raster/DXR slot bindings differ.");
        }
        CheckMembership(resources.m_blasRegistry.bottomLevels.size()==blasCount && resources.m_blasRegistry.bottomLevels.front().get()==blas,"Shared BLAS was rebuilt or replaced.");
        CheckMembership(resources.ValidateMaterialBindings(active->GetScene()),"Published material bindings invalid.");
    };
    for(int i=0;i<3;++i) render();
    SceneObjectDocument inserted=original.front(); inserted.id="membership-inserted"; inserted.name="Inserted";
    inserted.transform.position.x+=1.25f;
    SceneMembershipPublication insertion;
    auto* old=resources.m_membership.get();
    const auto oldDescriptor=old->instanceDataBuffer.GetDescriptorIndex();
    prepare([&] { return insertion.PrepareInsertion(r,*active,catalog,inserted,0,0); });
    // Deterministic lifetime check: candidate completes, then a tracked old-membership
    // submission is held behind a test fence. Publication must retain that snapshot.
    const auto candidateFence=context.GetLastSubmissionFenceValue();
    wait([&] { return context.GetCompletedFenceValue()>=candidateFence; });
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    CheckMembership(SUCCEEDED(r.m_graphicsDevice.GetDevice()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))),"Cannot create retirement test gate.");
    struct GateRelease { ID3D12Fence* gate; ~GateRelease() { gate->Signal(1); } } release{gate.Get()};
    CheckMembership(SUCCEEDED(context.GetCommandQueue()->Wait(gate.Get(),1)),"Cannot hold retirement test submission.");
    CheckMembership(r.BeginTrackedRecording(1) && r.EndTrackedRecording() && r.ExecuteTrackedRecording(1),"Cannot track old membership submission.");
    const auto oldFence=old->lastUseFence;
    const auto selected=publish(insertion,original.back().id);
    CheckMembership(selected && *selected==2 && engine.m_editor.GetSelectedObject()==2,"Selection did not follow shifted stable ID.");
    check({inserted.id,original[0].id,original[1].id});
    CheckMembership(active->GetScene().GetRenderObjects()[0].mesh==mesh &&
        active->GetObjectMetadata(inserted.id)->materialSlotBindings==inserted.materialSlotBindings,"Insertion lost shared mesh or authored material slots.");
    resources.CollectRetiredMemberships(context);
    const auto retained=std::find_if(resources.m_retiredMemberships.begin(),resources.m_retiredMemberships.end(),[&](const auto& item) { return item.get()==old; });
    CheckMembership(retained!=resources.m_retiredMemberships.end() && context.GetCompletedFenceValue()<oldFence &&
        old->instanceDataBuffer.GetResource() && old->instanceDataBuffer.GetDescriptorIndex()==oldDescriptor,"Old GPU snapshot retired before its fence.");
    CheckMembership(SUCCEEDED(gate->Signal(1)),"Cannot release retirement test gate.");
    wait([&] { return context.GetCompletedFenceValue()>=oldFence; });
    resources.CollectRetiredMemberships(context);
    CheckMembership(std::none_of(resources.m_retiredMemberships.begin(),resources.m_retiredMemberships.end(),[&](const auto& item) { return item.get()==old; }),"Completed snapshot was not collected.");
    for(int i=0;i<3;++i) render(selected);
    SceneMembershipPublication removal;
    prepare([&] { return removal.PrepareRemoval(r,*active,catalog,original[0].id,0); });
    const auto shifted=publish(removal,original.back().id);
    CheckMembership(shifted && *shifted==1,"Middle removal did not remap selection.");
    check({inserted.id,original[1].id});
    for(int i=0;i<3;++i) render(shifted);
    // Restore exact stable identity at an arbitrary index, then reject a stale transaction.
    SceneMembershipPublication restore;
    prepare([&] { return restore.PrepareInsertion(r,*active,catalog,original[0],1,0); });
    publish(restore,original[0].id); check({inserted.id,original[0].id,original[1].id});
    SceneMembershipPublication stale;
    prepare([&] { return stale.PrepareRemoval(r,*active,catalog,inserted.id,0); });
    auto transform=active->GetScene().GetRenderObjects()[1].transform; transform.position.y+=0.25f;
    active->GetScene().SetRenderObjectTransform(1,transform);
    std::optional<std::size_t> unchanged=123;
    const auto rejected=stale.PublishAtBoundary(r,active,catalog,original[1].id,unchanged);
    CheckMembership(rejected.status==Status::Stale && unchanged==123 && active->GetScene().GetRenderObjects().size()==3,"Stale transaction partially published.");
    stale.Cancel();
    for(int i=0;i<3;++i) render();
    // Valid but unadmitted materials retry; invalid references are terminal.
    auto unavailable=inserted; unavailable.id="membership-unavailable";
    unavailable.materialReference="asset:ffffffff-ffff-ffff-ffff-ffffffffffff";
    SceneMembershipPublication unavailableMaterial;
    CheckMembership(unavailableMaterial.PrepareInsertion(r,*active,catalog,unavailable,0,0).status==Status::Unsupported,
        "Invalid required material was not rejected terminally.");
    for(const auto& asset:engine.m_projectAssetRegistry.GetAssets())
    {
        if(asset.type!=AssetType::Material) continue;
        unavailable.materialReference=asset.id.ToReference();
        const auto result=unavailableMaterial.PrepareInsertion(r,*active,catalog,unavailable,0,0);
        CheckMembership(result.status==Status::Retry,"Unadmitted material did not retry without loading.");
        break;
    }
    // Unloaded geometry can join authored order without adding a DXR instance.
    for(const auto& asset:engine.m_projectAssetRegistry.GetAssets())
    {
        if(asset.type!=AssetType::Mesh) continue;
        auto pending=inserted; pending.id="membership-pending"; pending.meshReference=asset.id.ToReference();
        SceneMembershipPublication addPending;
        const auto instances=resources.m_membership->instances.size();
        prepare([&] { return addPending.PrepareInsertion(r,*active,catalog,pending,1,0); });
        publish(addPending,pending.id);
        CheckMembership(!active->GetScene().GetRenderObjects()[1].mesh && resources.m_membership->instances.size()==instances,
            "Pending mesh residency changed resident geometry or DXR instance count.");
        render(1);
        SceneMembershipPublication removePending;
        prepare([&] { return removePending.PrepareRemoval(r,*active,catalog,pending.id,0); });
        publish(removePending,original[1].id); render();
        break;
    }
    // Last resident/empty transitions use the same mechanism and no dummy instance.
    for(const auto& id:std::vector<std::string>{inserted.id,original[0].id,original[1].id})
    {
        SceneMembershipPublication erase;
        prepare([&] { return erase.PrepareRemoval(r,*active,catalog,id,0); });
        publish(erase,id); render();
    }
    check({});
    CheckMembership(resources.m_membership->instances.empty(),"Empty scene retained DXR instances.");
    SceneMembershipPublication fromEmpty;
    prepare([&] { return fromEmpty.PrepareInsertion(r,*active,catalog,original[1],0,0); });
    publish(fromEmpty,original[1].id); check({original[1].id});
    for(int i=0;i<3;++i) render(0);
    CheckMembership(context.GetGPUDrainCount()==drains,"Ordinary membership changes triggered a global GPU drain.");
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    CheckMembership(SUCCEEDED(r.m_graphicsDevice.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))),"D3D12 validation unavailable.");
    for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
    {
        SIZE_T size=0; queue->GetMessage(i,nullptr,&size); std::vector<std::byte> data(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        CheckMembership(SUCCEEDED(queue->GetMessage(i,message,&size)) && message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR &&
            message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"D3D12 validation error.");
    }
    Log::Info("Dynamic membership passed: front insertion, middle removal, stable-ID restoration, selection shifts, raster/DXR frames, shared BLAS reuse, stale rejection, cancellation, empty transitions and held-fence retirement; zero global GPU drains.");
}
};
bool Engine::VerifySceneMembership(const std::filesystem::path& path)
{
    ImGui::GetIO().IniFilename=nullptr;
    std::string error;
    if(!m_demoSceneManager.RequestSynchronousFileSceneDiagnostic(std::filesystem::absolute(path),&error) || !ActivateRequestedFileScene())
        throw std::runtime_error(error);
    auto* owner=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    CheckMembership(owner && !owner->IsArchiveActivation(),"Expected editor file scene.");
    CancelMeshResidencyAcceptance(); m_automaticMeshResidency={}; m_automaticResidencyIntent=0;
    SceneMembershipVerification::Run(*this,*owner);
    return true;
}
#endif
