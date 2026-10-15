#include "Engine/PackagedBootstrap.h"
#include "Engine/Engine.h"
#include "Platform/Windows/WindowsText.h"
#include "assets/CookedEnvironment.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/EngineAssetPak.h"
#include "Core/Log.h"
#include "Project/ProjectPaths.h"
#ifndef NDEBUG
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#endif
#include <stdexcept>
#include <fstream>
namespace PackagedBootstrap
{
void WriteStartup(const std::filesystem::path& path,const AssetID& scene,std::string_view title)
{
    std::string error;
    if(!title.empty() && !BuildGame::ValidateTitle(title,error)) throw std::runtime_error(error);
    std::ofstream f(path,std::ios::binary);
    f << "ENGINE_GAME_STARTUP 2\n" << scene.ToString() << "\n" << title << "\n"; f.close();
    if(!f) throw std::runtime_error("Cannot write packaged startup manifest.");
}
Startup ReadStartupConfiguration(const std::filesystem::path& path)
{
    if(std::filesystem::file_size(path)>256) throw std::runtime_error("Packaged startup manifest is too large.");
    std::ifstream f(path,std::ios::binary); std::string header,id,title,extra,error;
    if(!std::getline(f,header) || (header!="ENGINE_GAME_STARTUP 1" && header!="ENGINE_GAME_STARTUP 2") ||
        !std::getline(f,id) || !AssetID::Parse(id))
        throw std::runtime_error("Invalid packaged startup manifest header or scene UUID.");
    if(header=="ENGINE_GAME_STARTUP 2" && (!std::getline(f,title) || (!title.empty() && !BuildGame::ValidateTitle(title,error))))
        throw std::runtime_error("Invalid packaged startup title: "+error);
    if(std::getline(f,extra)) throw std::runtime_error("Unexpected packaged startup data.");
    return {*AssetID::Parse(id),std::move(title)};
}
AssetID ReadStartup(const std::filesystem::path& path) { return ReadStartupConfiguration(path).scene; }
void Cook(const std::filesystem::path& inputTOC,const std::filesystem::path& inputPak,
    const std::filesystem::path& outputTOC,const std::filesystem::path& outputPak,const std::filesystem::path& shaderRoot)
{
    auto archive=EngineAssetPak::Reader::Open(inputTOC,inputPak);
    std::vector<std::vector<std::byte>> owned;
    owned.reserve(archive.Entries().size()+1);
    std::vector<EngineAssetPak::PreparedAsset> assets;
    size_t bytes=0;
    for(const auto& entry:archive.Entries())
    {
        if(entry.id==ShaderID()) throw std::runtime_error("Bootstrap ID already exists in input archive.");
        if(entry.storedBytes>512*1024*1024-bytes) throw std::runtime_error("Bootstrap cook batch exceeds 512 MiB.");
        owned.push_back(archive.Read(entry.id)); bytes+=owned.back().size();
        assets.push_back({entry.id,entry.type,entry.payloadVersion,entry.generation,owned.back(),entry.alignment});
    }
    owned.push_back(ShaderLibrary::CookBootstrap(shaderRoot));
    if(owned.back().size()>64*1024*1024) throw std::runtime_error("Shader bundle exceeds 64 MiB.");
    assets.push_back({ShaderID(),EngineAssetPak::Type::Shader,1,1,owned.back(),16});
    EngineAssetPak::Write(outputTOC,outputPak,assets);
    Log::Info("Packaged bootstrap archive written; shaders compiled offline. Runtime scene IDs are unchanged.");
}
}
void Engine::VerifyPackagedBootstrap()
{
    if(m_startup.mode!=EngineStartup::Mode::PackagedRuntime) throw std::runtime_error("Diagnostic requires packaged startup.");
    auto* active=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    if(!active || !active->ArchiveSource() || m_renderer.GetEnvironmentIdentity()!=CookedEnvironment::ArchiveIdentity(*active->ArchiveSource()))
        throw std::runtime_error("Packaged renderer environment evidence differs from the archived scene.");
    Log::Info("Verified packaged scene environment: "+m_renderer.GetEnvironmentIdentity());
    const ProjectPaths forbidden("packaged-forbidden-project");
    std::uint64_t ticket=0; std::string error;
    const auto id=*m_startup.sceneID;
    if(m_texturePreparation.Request(forbidden,m_projectAssetRegistry,id,ProjectTextureInterpretation::LinearData,{},
        m_texturePreparation.Generation(),ticket,error) || error.find("packaged policy")==std::string::npos)
        throw std::runtime_error("Loose texture provider policy failed.");
    if(m_meshPreparation.Request(forbidden,m_projectAssetRegistry,id,m_meshPreparation.Generation()).error.find("packaged policy")==std::string::npos)
        throw std::runtime_error("Loose mesh provider policy failed.");
    if(m_materialPreparation->Request(forbidden,m_projectAssetRegistry,id,1,ticket,error)!=ProjectMaterialPreparationService::Status::Unsupported ||
        error.find("packaged policy")==std::string::npos) throw std::runtime_error("Loose material provider policy failed.");
    const auto hashes=m_texturePreparation.GetStatistics().workerHashes;
    auto& scene=m_demoSceneManager.GetActiveScene().GetScene();
    for(const auto& object:scene.GetRenderObjects())
        if(object.materialsDeferred) throw std::runtime_error("Packaged bootstrap left deferred material bindings.");
    // Exercise the shared raster/DXR frame without beginning an editor/ImGui frame.
    for(unsigned int frame=0;frame<3;++frame)
    {
        if(m_renderer.Render(scene,RendererOptions{})==Renderer::FrameRenderResult::Error)
            throw std::runtime_error("Packaged bootstrap frame failed.");
    }
    // Exercise actual WM_SIZE dispatch, scene-target resize and the normal Frame path.
    const auto window=static_cast<HWND>(m_window.GetNativeHandle());
    const auto checkCaption=[&]() {
        const auto expected=WindowsText::FromUTF8(m_startup.gameTitle.empty() ? "Engine Packaged Runtime" : m_startup.gameTitle);
        std::wstring actual(GetWindowTextLengthW(window)+1,L'\0');
        actual.resize(GetWindowTextW(window,actual.data(),static_cast<int>(actual.size())));
        if(actual!=expected) throw std::runtime_error("Packaged Win32 window caption differs from startup title.");
    };
    checkCaption();
    if(!SetWindowPos(window,nullptr,0,0,1024,640,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
        throw std::runtime_error("Packaged verification could not resize its window.");
    for(unsigned int frame=0;frame<3;++frame) Frame();
    Renderer::FinalSceneImage image{};
    if(m_window.ShouldClose() || !m_renderer.TryGetFinalSceneImage(image) ||
        image.width!=m_window.GetWidth() || image.height!=m_window.GetHeight())
        throw std::runtime_error("Packaged resize did not retain coherent scene/window dimensions.");
    checkCaption();
    Log::Info("Verified Win32 window caption before and after resize.");
    // The normal application loop must consume WM_CLOSE without asking the editor.
    if(!PostMessageW(window,WM_CLOSE,0,0)) throw std::runtime_error("Cannot queue packaged window closure.");
    Run();
    if(!m_window.ShouldClose()) throw std::runtime_error("Packaged loop did not handle window closure.");
    if(hashes!=0 || m_texturePreparation.GetStatistics().workerHashes!=hashes)
        throw std::runtime_error("Unexpected loose texture hashing in packaged mode.");
#ifndef NDEBUG
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
    if(FAILED(m_renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue)))) throw std::runtime_error("D3D12 validation unavailable.");
    for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
    {
        SIZE_T size=0; queue->GetMessage(i,nullptr,&size); std::vector<std::byte> data(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(FAILED(queue->GetMessage(i,message,&size)) || message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)
            throw std::runtime_error("D3D12 error during packaged bootstrap.");
    }
#endif
    Log::Info("Packaged bootstrap passed: renderer, archive scene, material/texture admission, mesh residency and runtime frames, resize and window-close loop; no editor/project provider or HLSL source access.");
}
