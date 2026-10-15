#include "Engine/Engine.h"
#include "Engine/PackagedBootstrap.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/CookedScene.h"
#include "assets/CookedSurface.h"
#include "Core/Log.h"
#include <fstream>
#include <thread>
#include <stdexcept>
namespace
{
void CheckBuild(bool ok,const std::string& error) { if(!ok) throw std::runtime_error(error); }
std::vector<std::byte> Bytes(const std::filesystem::path& path)
{
    std::ifstream f(path,std::ios::binary|std::ios::ate); CheckBuild(bool(f),"Cannot inspect diagnostic output.");
    auto size=f.tellg(); CheckBuild(size>=0 && size<256*1024*1024,"Diagnostic output too large.");
    std::vector<std::byte> bytes(static_cast<size_t>(size)); f.seekg(0); f.read(reinterpret_cast<char*>(bytes.data()),bytes.size()); CheckBuild(bool(f),"Cannot read diagnostic output."); return bytes;
}
}
bool Engine::VerifyBuildGame(const std::filesystem::path& sourcePath,const std::filesystem::path& output)
{
    std::string error;
    for(auto title:{"", "CON", "LPT9.txt", "foo/evil", "name.", "COM1 .txt"}) CheckBuild(!BuildGame::ValidateTitle(title,error),"Unsafe title accepted.");
    CheckBuild(m_demoSceneManager.RequestMetadataOnlyFileScene(std::filesystem::absolute(sourcePath),&error),error);
    CheckBuild(ActivateRequestedFileScene(),"Cannot activate current Build Game scene.");
    auto* selected=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    CheckBuild(selected && !selected->IsArchiveActivation() && selected->GetScenePath()==std::filesystem::absolute(sourcePath),"Diagnostic did not select requested current scene.");
    const auto sceneID=BuildGame::SceneIdentity(selected->GetPersistentSceneID());
    CheckBuild(StartBuildGame({"C37Game",output,false},error),error);
    CheckBuild(!StartBuildGame({"Overlap",output,false},error),"Overlapping build accepted.");
    const auto wait=[&]()
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(3);
        while(m_buildGame.Poll().busy && std::chrono::steady_clock::now()<deadline)
        { Frame(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CheckBuild(!m_buildGame.Poll().busy,"Build workflow timed out.");
    };
    wait(); CheckBuild(m_buildGame.Poll().success,m_buildGame.Poll().message);
    CheckBuild(std::filesystem::exists(output/"C37Game.exe") && !std::filesystem::exists(output/"EngineRuntime.exe"),"Executable title was not applied.");
    const auto config=PackagedBootstrap::ReadStartupConfiguration(output/"Game.startup");
    CheckBuild(config.scene==sceneID && config.title=="C37Game","Startup scene/title differs from build settings.");
    { const auto archive=CookedScene::OpenArchive(output/"Game.toc",output/"Game.eap",sceneID); CheckBuild(archive->metadata.document.id==selected->GetPersistentSceneID(),"Persistent scene identity changed."); }
    const auto toc=Bytes(output/"Game.toc"),pak=Bytes(output/"Game.eap"),startup=Bytes(output/"Game.startup");
    BuildGame::Request failure{{"C37Game",output,false},selected->m_runtime->GetFileScene().GetDocument(),m_projectAssetRegistry,
        selected->GetProjectRoot(),WindowsWindow::GetExecutableDirectory()/"assets"/"Shaders",output/"missing-runtime"};
    CheckBuild(m_buildGame.Start(std::move(failure),error),error); wait();
    CheckBuild(!m_buildGame.Poll().success,"Missing runtime build unexpectedly succeeded.");
    CheckBuild(toc==Bytes(output/"Game.toc") && pak==Bytes(output/"Game.eap") && startup==Bytes(output/"Game.startup") &&
        std::filesystem::exists(output/"C37Game.exe"),"Failed staged build changed successful deployment.");
    Log::Info("Build Game workflow passed: current scene saved/cooked, editor frames continued, title C37Game.exe, stable startup scene, overlap rejected and failed staging preserved previous output. Deployment: "+output.string());
    return true;
}
