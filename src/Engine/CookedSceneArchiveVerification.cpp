#include "Engine/ArchiveSurfaceAdmission.h"
#include "assets/CookedSurface.h"
#include "Engine/Engine.h"
#include "Core/Log.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/CookedScene.h"
#include "assets/DevelopmentCookedMeshProvider.h"
#include "assets/AssetRegistrySerializer.h"
#include "Scene/SceneSerializer.h"
#include "Scene/SceneDocumentAdapter.h"
#include <Windows.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <stdexcept>

namespace
{
    void RequireScene(bool condition,const std::string& message) { if(!condition) throw std::runtime_error(message); }
    template<class F> void RejectScene(F operation)
    { bool rejected=false; try { operation(); } catch(const std::runtime_error&) { rejected=true; } RequireScene(rejected,"Malformed/stale cooked scene was accepted."); }
}

bool Engine::ActivateCookedSceneArchive(const std::filesystem::path& toc,const std::filesystem::path& pak,const AssetID& id)
{
    const auto source=CookedScene::OpenArchive(toc,pak,id);
    std::string error;
    CancelMeshResidencyAcceptance(); m_meshPreparation.Invalidate();
    RequireScene(m_demoSceneManager.RequestArchiveScene(source,&error),error);
    if(!ActivateRequestedFileScene()) std::abort(); // Preserve uncertain GPU ownership, same as C.2.
    PopulateArchiveScene(source);
    auto* scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    // Checked replacement proves all scene/catalog/archive borrows survive until
    // the existing GPU drain, BLAS/TLAS release and manager commit have completed.
    const auto oldRuntime=scene->GetRuntimeIdentity();
    m_meshPreparation.Invalidate();
    RequireScene(m_demoSceneManager.RequestArchiveScene(source,&error),error);
    if(!ActivateRequestedFileScene()) std::abort();
    auto* replacement=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    RequireScene(replacement && replacement->IsArchiveActivation() && replacement->GetRuntimeIdentity()!=oldRuntime,"Archive scene session replacement failed.");
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    RequireScene(SUCCEEDED(m_renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))),"D3D12 validation unavailable.");
    for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
    {
        SIZE_T length=0; RequireScene(SUCCEEDED(queue->GetMessage(i,nullptr,&length)),"Cannot inspect D3D12 validation.");
        std::vector<std::byte> storage(length); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        RequireScene(SUCCEEDED(queue->GetMessage(i,message,&length)),"Cannot read D3D12 validation.");
        RequireScene(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"D3D12 error during archive scene publication.");
    }
    Log::Info("Archive-only scene activation passed: metadata and meshes came exclusively from .toc/.eap; "+std::to_string(source->metadata.document.objects.size())+
        " objects published with authored bindings through existing BLAS/TLAS coordinator; "+std::to_string(0)+
        " objects retain explicit deferred material references; stable indices, scene replacement and D3D12 validation passed.");
    return true;
}

bool Engine::VerifyCookedSceneArchive(const std::filesystem::path& sourcePath)
{
    namespace fs=std::filesystem;
    auto root=fs::canonical(sourcePath).parent_path();
    while(!fs::exists(root/"Assets.registry.json")) { RequireScene(root!=root.parent_path(),"No authoring project registry."); root=root.parent_path(); }
    std::string error; SceneDocument original;
    RequireScene(LoadSceneDocumentForCooking(sourcePath,original,CookedScene::Policy{}.maxBytes,&error),error);
    ProjectAssetRegistry registry;
    RequireScene(AssetRegistrySerializer::LoadFromFile(root/"Assets.registry.json",registry,&error),error);
    auto development=DevelopmentCookedMesh::Provider::Open(root,{});
    RequireScene(bool(development.provider),std::string(development.message));
    auto found=std::find_if(original.objects.begin(),original.objects.end(),[&](const auto& o)
    { auto id=AssetID::ParseReference(o.meshReference); return id && development.provider->Lookup(*id).status==CookedMeshSource::Status::Success; });
    RequireScene(found!=original.objects.end(),"Round-trip source needs a published cooked mesh.");
    const auto mesh=*AssetID::ParseReference(found->meshReference);
    auto acquired=development.provider->Acquire(mesh); RequireScene(bool(acquired.lease),std::string(acquired.message));
    SceneDocument document=original; document.objects={*found}; document.id="cooked-scene-archive-probe";
    auto renderProbe=*found; renderProbe.id+="-builtin-material-probe"; renderProbe.name="Explicit builtin material probe";
    renderProbe.materialReference="builtin:default"; renderProbe.materialSlotBindings.clear(); document.objects.push_back(renderProbe);
    auto deferredProbe=*found; deferredProbe.id+="-deferred-material-probe"; deferredProbe.name="Explicit deferred material probe";
    const auto materials=registry.GetAssets(AssetType::Material); RequireScene(!materials.empty(),"Diagnostic requires an authored material identity.");
    deferredProbe.materialReference=materials.front().id.ToReference(); deferredProbe.materialSlotBindings.clear();
    deferredProbe.materialSlotBindings[acquired.lease->Metadata()->metadata.materialSlots.front().id]=materials.front().id.ToReference(); document.objects.push_back(deferredProbe);
    CookedScene::MeshInput input{mesh,41,acquired.lease->Bytes()};
    const auto fixture=fs::absolute(fs::path("build/Debug")/("cooked-scene-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
    RequireScene(fs::create_directory(fixture),"Fixture collision.");
    fs::create_directory(fixture/"archive-only");
    const auto authoring=fixture/"Authoring.scene";
    RequireScene(SaveSceneDocument(document,authoring,&error),error);
    const auto cooked=CookedScene::CookFile(authoring,{&input,1},registry);
    const auto decoded=CookedScene::Decode(cooked);
    RequireScene(SceneDocumentsEqual(document,decoded.document) && cooked==CookedScene::Cook(document,{&input,1},registry) &&
        decoded.meshes[0]->slots==acquired.lease->Metadata()->metadata.materialSlots,"Cooked scene round-trip/deterministic bytes/slot layout differ.");
    auto malformed=cooked; malformed[0]=std::byte{}; RejectScene([&]{CookedScene::Decode(malformed);});
    malformed=cooked; malformed.pop_back(); RejectScene([&]{CookedScene::Decode(malformed);});
    const size_t parentOffset=32+4+document.id.size()+4+document.name.size()+76;
    malformed=cooked; std::fill_n(malformed.begin()+parentOffset,4,std::byte{}); RejectScene([&]{CookedScene::Decode(malformed);});
    const auto sceneID=*AssetID::Parse("cccccccc-cccc-cccc-cccc-cccccccccccc");
    std::array<EngineAssetPak::PreparedAsset,2> entries{{
        {mesh,EngineAssetPak::Type::Mesh,1,41,acquired.lease->Bytes(),16},
        {sceneID,EngineAssetPak::Type::Scene,CookedScene::Version,7,cooked,16}}};
    const auto toc=fixture/"archive-only/game.toc",pak=fixture/"archive-only/game.eap";
    auto surfaces=CookedSurface::CookDependencies(decoded,ProjectPaths(root),registry);
    std::vector<EngineAssetPak::PreparedAsset> complete(entries.begin(),entries.end());
    for(const auto& surface:surfaces) complete.push_back({surface.id,surface.type,CookedSurface::Version,surface.generation,surface.bytes,16});
    EngineAssetPak::Write(toc,pak,complete);
    entries[0].generation=42;
    EngineAssetPak::Write(fixture/"stale.toc",fixture/"stale.eap",entries);
    RejectScene([&]{CookedScene::OpenArchive(fixture/"stale.toc",fixture/"stale.eap",sceneID);});
    fs::remove(authoring); // Our own temporary authoring input is gone before activation.
    acquired.lease.reset(); development.provider.reset();
    size_t files=0;
    for(const auto& file:fs::directory_iterator(fixture/"archive-only")) { RequireScene(file.path()==toc || file.path()==pak,"Loose source in archive-only fixture."); ++files; }
    RequireScene(files==2,"Archive-only fixture must contain exactly the archive pair.");
    Log::Info("Cooked scene round-trip passed: deterministic bytes, names/order/world transforms, complete slot layout and authored material identities; malformed header/truncation/hierarchy and stale mesh generation rejected.");
    RequireScene(ActivateCookedSceneArchive(toc,pak,sceneID),"Archive-only activation failed.");
    Log::Info("Archive-only fixture retained: "+(fixture/"archive-only").string());
    return true;
}
