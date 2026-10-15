#include "Engine/Engine.h"
#include "Engine/PackagedBootstrap.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedScene.h"
#include "assets/ProjectTextureImportService.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"
#include <fstream>
#include "imgui.h"
#include <stdexcept>
namespace
{
void RequireHDR(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); }
template<class F> void RejectHDR(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } RequireHDR(rejected,"Missing/corrupt authored HDR environment was accepted."); }
}
bool Engine::VerifySceneHDRI(const std::filesystem::path& sourcePath,const std::filesystem::path& output)
{
    namespace fs=std::filesystem; std::string error;
    ImGui::GetIO().IniFilename=nullptr; // Diagnostic frames must not overwrite the user workspace layout.
    fs::create_directories(output.parent_path());
    const auto hdr=output.parent_path()/"C38Assigned.hdr";
    { std::ofstream f(hdr,std::ios::binary); f << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8 +X 16\n"; const unsigned char pixel[]{128,64,32,130}; for(int i=0;i<128;++i) f.write(reinterpret_cast<const char*>(pixel),4); RequireHDR(bool(f),"Cannot create diagnostic HDR source."); }
    RequireHDR(m_demoSceneManager.RequestMetadataOnlyFileScene(fs::absolute(sourcePath),&error),error);
    RequireHDR(ActivateRequestedFileScene(),"Cannot activate HDR diagnostic scene.");
    auto* scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene()); RequireHDR(scene && !scene->IsArchiveActivation(),"Expected loose editor scene.");
    const auto destination=scene->GetProjectRoot()/"Textures"/"Environment"; fs::create_directories(destination); fs::path imported;
    RequireHDR(ProjectTextureImportService::Import(ProjectPaths(scene->GetProjectRoot()),m_projectAssetRegistry,hdr,destination,imported,error),error);
    const auto* record=m_projectAssetRegistry.FindByPath(fs::relative(imported,scene->GetProjectRoot())); RequireHDR(record!=nullptr,"HDR import did not register an identity.");
    const auto id=record->id; const auto reference=id.ToReference();
    scene->GetScene().SetEnvironmentReference(reference); RequireHDR(ApplySceneEnvironment(error),error);
    const auto assignedIndex=m_renderer.GetCurrentEnvironmentIndex();
    const auto& choices=m_renderer.GetAvailableEnvironments(); size_t neutralIndex=choices.size();
    for(size_t i=0;i<choices.size();++i) if(choices[i].relativePath.empty() && choices[i].reference.empty() && choices[i].displayName=="Neutral") neutralIndex=i;
    RequireHDR(neutralIndex<choices.size() && AssignSceneEnvironment(neutralIndex,error),error);
    RequireHDR(scene->GetScene().GetEnvironmentReference().empty(),"Neutral selection retained an authored HDR reference.");
    RequireHDR(AssignSceneEnvironment(assignedIndex,error),error); RequireHDR(scene->Save(&error),error);
    SceneDocument saved; RequireHDR(LoadSceneDocumentForCooking(sourcePath,saved,CookedScene::Policy{}.maxBytes,&error),error); RequireHDR(saved.environmentReference==reference,"HDR reference did not survive scene save.");
    const auto neutralPath=sourcePath.parent_path()/"C38NeutralDiagnostic.scene"; auto neutral=saved; neutral.environmentReference.clear();
    RequireHDR(SaveSceneDocument(neutral,neutralPath,&error),error);
    RequireHDR(m_demoSceneManager.RequestMetadataOnlyFileScene(neutralPath,&error) && ActivateRequestedFileScene(),error);
    RequireHDR(m_renderer.GetEnvironmentIdentity()=="neutral","Unassigned scene inherited another scene's HDRI.");
    RequireHDR(m_demoSceneManager.RequestMetadataOnlyFileScene(sourcePath,&error) && ActivateRequestedFileScene(),error);
    scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    RequireHDR(scene && scene->GetScene().GetEnvironmentReference()==reference && m_renderer.GetEnvironmentIdentity()!="neutral","Scene reload did not restore its authored HDRI.");
    RequireHDR(VerifyBuildGame(sourcePath,output),"HDR scene Build Game failed.");
    const auto sceneID=BuildGame::SceneIdentity(saved.id);
    { auto archived=CookedScene::OpenArchive(output/"Game.toc",output/"Game.eap",sceneID); auto pixels=CookedEnvironment::ReadArchive(*archived); const auto expected=CookedEnvironment::ReadLoose(imported);
      RequireHDR(pixels && pixels->width==expected.width && pixels->height==expected.height && pixels->pixels==expected.pixels,"Packaged HDR linear radiance differs from the assigned source."); }
    auto reader=EngineAssetPak::Reader::Open(output/"Game.toc",output/"Game.eap");
    std::vector<std::vector<std::byte>> bytes; bytes.reserve(reader.Entries().size()); std::vector<EngineAssetPak::PreparedAsset> entries;
    const auto* environment=reader.Lookup(id); RequireHDR(environment && environment->type==EngineAssetPak::Type::Resource,"Build omitted required HDR resource."); const auto offset=environment->offset;
    for(const auto& entry:reader.Entries()) if(entry.id!=id) { bytes.push_back(reader.Read(entry.id)); entries.push_back({entry.id,entry.type,entry.payloadVersion,entry.generation,bytes.back(),entry.alignment}); }
    const auto missingTOC=output.parent_path()/"missing-hdr.toc", missingPak=output.parent_path()/"missing-hdr.eap";
    EngineAssetPak::Write(missingTOC,missingPak,entries);
    RejectHDR([&]{ CookedScene::OpenArchive(missingTOC,missingPak,sceneID); });
    const auto corruptTOC=output.parent_path()/"corrupt-hdr.toc", corruptPak=output.parent_path()/"corrupt-hdr.eap";
    fs::copy_file(output/"Game.toc",corruptTOC); fs::copy_file(output/"Game.eap",corruptPak);
    { std::fstream f(corruptPak,std::ios::binary|std::ios::in|std::ios::out); f.seekg(static_cast<std::streamoff>(offset+72)); char b=0; f.read(&b,1); b^=1; f.seekp(static_cast<std::streamoff>(offset+72)); f.write(&b,1); RequireHDR(bool(f),"Cannot create corrupt HDR diagnostic archive."); }
    RejectHDR([&]{ auto source=CookedScene::OpenArchive(corruptTOC,corruptPak,sceneID); CookedEnvironment::ReadArchive(*source); });
    Log::Info("Scene HDRI passed: stable project identity, save/reload, per-scene neutral/restoration, Build Game linear radiance equality and missing/corrupt required HDR rejection. Deployment: "+output.string()); return true;
}
