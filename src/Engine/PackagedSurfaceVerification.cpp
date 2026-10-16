#include "Engine/Engine.h"
#include "Core/Log.h"
#include "assets/CookedSurface.h"
#include "assets/DevelopmentCookedMeshProvider.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/MaterialAssetSerializer.h"
#include "Scene/SceneSerializer.h"
#include <Windows.h>
#include <fstream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <stdexcept>

bool Engine::VerifyArchiveMaterials(const std::filesystem::path& authoringScene)
{
    namespace fs=std::filesystem;
    const auto check=[](bool ok,const std::string& message){ if(!ok) throw std::runtime_error("Packaged surface diagnostic: "+message); };
    auto root=fs::canonical(authoringScene).parent_path();
    while(!fs::exists(root/"Assets.registry.json")) { check(root!=root.parent_path(),"Cannot locate authoring project."); root=root.parent_path(); }
    std::string error; SceneDocument source;
    check(LoadSceneDocumentForCooking(authoringScene,source,CookedScene::Policy{}.maxBytes,&error),error);
    auto loose=DevelopmentCookedMesh::Provider::Open(root,{}); check(bool(loose.provider),std::string(loose.message));
    const auto selected=std::find_if(source.objects.begin(),source.objects.end(),[&](const auto& o){ auto id=AssetID::ParseReference(o.meshReference); return id && loose.provider->Lookup(*id).status==CookedMeshSource::Status::Success; });
    check(selected!=source.objects.end(),"A published cooked mesh is required for the authoring probe.");
    const auto meshID=*AssetID::ParseReference(selected->meshReference);
    auto acquired=loose.provider->Acquire(meshID); check(bool(acquired.lease),std::string(acquired.message));
    auto decoded=CookedMesh::Decode(acquired.lease->Bytes(),{}); check(bool(decoded.geometry),"Cannot prepare authoring mesh.");
    ImportedMesh geometry=*decoded.geometry;
    const auto first=static_cast<std::uint32_t>((geometry.indices.size()/6)*3);
    check(first && first<geometry.indices.size(),"Diagnostic mesh needs at least two triangles.");
    geometry.metadata.materialSlots={{"packaged:0","Packaged fallback",0},{"packaged:1","Packaged override",1}};
    geometry.metadata.submeshes={{0,first,0,"Fallback"},{first,static_cast<std::uint32_t>(geometry.indices.size()-first),1,"Override"}};
    auto mesh=CookedMesh::Encode(geometry,{}); check(mesh.status==CookedMesh::Status::Success,"Cannot encode multi-slot authoring mesh.");
    acquired.lease.reset(); loose.provider.reset(); decoded.geometry.reset();
    const auto fixture=fs::absolute(fs::path("build/Debug")/("packaged-surfaces-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
    check(fs::create_directory(fixture),"Fixture collision.");
    const ProjectPaths paths(fixture); check(paths.EnsureDirectories(&error),error); fs::create_directory(fixture/"archive-only");
    const auto materialA=*AssetID::Parse("dddddddd-dddd-dddd-dddd-dddddddddd01"),materialB=*AssetID::Parse("dddddddd-dddd-dddd-dddd-dddddddddd02");
    const auto textureID=*AssetID::Parse("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee"),sceneID=*AssetID::Parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
    ProjectAssetRegistry registry;
    check(registry.Register({materialA,AssetType::Material,"Materials/A.material"}) && registry.Register({materialB,AssetType::Material,"Materials/B.material"}) &&
        registry.Register({textureID,AssetType::Texture,"Textures/Shared.tga"}),"Cannot register source fixture assets.");
    MaterialAssetData a,b; a.baseColor={0.8f,0.4f,0.2f}; b.baseColor={0.2f,0.6f,0.8f}; a.baseColorTexture=textureID.ToReference(); b.baseColorTexture=textureID.ToReference();
    check(MaterialAssetSerializer::SaveToFile(a,fixture/"Materials/A.material",&error) && MaterialAssetSerializer::SaveToFile(b,fixture/"Materials/B.material",&error),error);
    std::array<unsigned char,18+16*16*3> tga{}; tga[2]=2; tga[12]=16; tga[14]=16; tga[16]=24; tga[17]=32;
    for(size_t i=18;i<tga.size();++i) tga[i]=static_cast<unsigned char>((i*17)%255);
    { std::ofstream out(fixture/"Textures/Shared.tga",std::ios::binary); out.write(reinterpret_cast<const char*>(tga.data()),tga.size()); check(bool(out),"Cannot write source texture probe."); }
    SceneDocument document=source; document.id="packaged-surface-probe"; document.objects={*selected};
    document.objects[0].materialReference=materialA.ToReference(); document.objects[0].materialSlotBindings={{"packaged:1",materialB.ToReference()}};
    const CookedScene::MeshInput input{meshID,11,mesh.payload};
    auto sceneBytes=CookedScene::Cook(document,{&input,1},registry); auto scene=CookedScene::Decode(sceneBytes);
    auto cooked=CookedSurface::CookDependencies(scene,paths,registry,19);
    auto again=CookedSurface::CookDependencies(scene,paths,registry,19);
    check(cooked.size()==3 && cooked.size()==again.size(),"Shared texture was duplicated or a dependency was omitted.");
    for(size_t i=0;i<cooked.size();++i) check(cooked[i].id==again[i].id && cooked[i].bytes==again[i].bytes,"Nondeterministic cooked surface output.");
    std::vector<EngineAssetPak::PreparedAsset> entries{{meshID,EngineAssetPak::Type::Mesh,1,11,mesh.payload,16},{sceneID,EngineAssetPak::Type::Scene,CookedScene::Version,13,sceneBytes,16}};
    for(const auto& asset:cooked) entries.push_back({asset.id,asset.type,CookedSurface::Version,asset.generation,asset.bytes,16});
    const auto toc=fixture/"archive-only/game.toc",pak=fixture/"archive-only/game.eap"; EngineAssetPak::Write(toc,pak,entries);
    if(!m_materialPreparation) m_materialPreparation=std::make_unique<ProjectMaterialPreparationService>(m_texturePreparation);
    const auto prepare=[&](const std::shared_ptr<const CookedMeshSource::ArchiveProvider>& provider,std::uint64_t session)
    {
        std::uint64_t ticket=0;
        check(m_materialPreparation->RequestArchive(provider,materialA,session,ticket,error)==ProjectMaterialPreparationService::Status::Pending,error);
        auto result=m_materialPreparation->PollArchive(ticket,session);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while((result.status==ProjectMaterialPreparationService::Status::Pending || result.status==ProjectMaterialPreparationService::Status::Retry) && std::chrono::steady_clock::now()<deadline)
        { std::this_thread::sleep_for(std::chrono::milliseconds(2)); result=m_materialPreparation->PollArchive(ticket,session); }
        return std::make_pair(ticket,result);
    };
    auto missing=entries; std::erase_if(missing,[&](const auto& e){return e.id==textureID;});
    EngineAssetPak::Write(fixture/"missing.toc",fixture/"missing.eap",missing);
    {
        auto broken=CookedScene::OpenArchive(fixture/"missing.toc",fixture/"missing.eap",sceneID); auto [ticket,result]=prepare(broken->provider,71);
        check(result.status==ProjectMaterialPreparationService::Status::Stale || result.status==ProjectMaterialPreparationService::Status::Failed,"Missing archived texture dependency was accepted."); m_materialPreparation->Cancel(ticket);
    }
    EngineAssetPak::Write(fixture/"corrupt.toc",fixture/"corrupt.eap",entries);
    std::uint64_t offset=0;
    { auto reader=EngineAssetPak::Reader::Open(fixture/"corrupt.toc",fixture/"corrupt.eap"); offset=reader.Lookup(textureID)->offset; }
    { std::fstream file(fixture/"corrupt.eap",std::ios::binary|std::ios::in|std::ios::out); file.seekg(offset); char byte{}; file.read(&byte,1); byte^=1; file.seekp(offset); file.write(&byte,1); check(bool(file),"Cannot corrupt dependency probe."); }
    {
        auto broken=CookedScene::OpenArchive(fixture/"corrupt.toc",fixture/"corrupt.eap",sceneID); auto [ticket,result]=prepare(broken->provider,72);
        check(result.status==ProjectMaterialPreparationService::Status::Failed,"Corrupt archived texture dependency was accepted."); m_materialPreparation->Cancel(ticket);
    }
    {
        auto valid=CookedScene::OpenArchive(toc,pak,sceneID); auto [ticket,result]=prepare(valid->provider,73);
        check(result.status==ProjectMaterialPreparationService::Status::Ready && result.candidate,"Valid archive surface preparation failed: "+result.message);
        check(!m_materialPreparation->ValidateArchive(*result.candidate,74,error),"Stale scene session accepted.");
        auto pixels=result.candidate->textures[0]->prepared; m_materialPreparation->Cancel(ticket);
        check(!m_materialPreparation->ValidateArchive(*result.candidate,73,error) && pixels && !pixels->pixels.empty(),"Cancellation lost owned pixels or retained authorization.");
    }
    fs::remove(fixture/"Materials/A.material"); fs::remove(fixture/"Materials/B.material"); fs::remove(fixture/"Textures/Shared.tga");
    check(fs::is_empty(fixture/"Materials") && fs::is_empty(fixture/"Textures"),"Loose surface sources remain.");
    const auto before=m_texturePreparation.GetStatistics().workerHashes;
    check(ActivateCookedSceneArchive(toc,pak,sceneID),"Archive-only textured activation failed.");
    check(m_texturePreparation.GetStatistics().workerHashes==before,"Packaged runtime used loose development hashing.");
    Log::Info("Packaged material/texture diagnostic passed: deterministic cooked payloads; two authored multi-slot materials, one shared GPU texture, asynchronous upload and append-only admission; missing/corrupt dependencies, stale session and cancellation rejected; no loose material/texture files or runtime file hashing. Fixture: "+(fixture/"archive-only").string());
    return true;
}
