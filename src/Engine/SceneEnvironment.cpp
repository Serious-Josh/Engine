#include "Engine/Engine.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ProjectTextureImportService.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/CookedScene.h"
#include "Core/Log.h"
namespace
{
bool LoadProjectEnvironment(Renderer& renderer,const std::filesystem::path& root,const ProjectAssetRegistry& registry,const std::string& reference,
    float radianceScale)
{
        const auto id=AssetID::ParseReference(reference);
        const auto* record=id?registry.Find(*id):nullptr;
        const auto resolved=id?ProjectAssetResolver::Resolve(ProjectPaths(root),registry,*id):AssetResolution{};
        if(!record || record->type!=AssetType::Texture || resolved.status!=AssetAvailability::Available)
            throw std::runtime_error("Required scene HDRI asset is missing or invalid.");
        // Re-read on explicit assignment/scene replacement. No development file enters a packaged provider.
        auto image=CookedEnvironment::ReadLoose(resolved.resolvedPath);
        const auto hash=CookedSurface::Hash(std::as_bytes(std::span(image.pixels)));
        std::string identity="project:"+std::filesystem::canonical(root).generic_string()+":"+id->ToString()+":"+std::to_string(image.width)+"x"+std::to_string(image.height)+":";
        constexpr char hex[]="0123456789abcdef"; for(auto b:hash) { auto v=std::to_integer<unsigned>(b); identity+=hex[v>>4]; identity+=hex[v&15]; }
        return renderer.SetEnvironmentImage(&image,identity,resolved.resolvedPath.stem().string(),reference,radianceScale);
}
}
// The active scene's HDRI calibration (scene environment luminance scale times its render pre-scale).
float Engine::ActiveEnvironmentRadianceScale() const
{
    const auto* scene=m_demoSceneManager.TryGetActiveScene();
    return EnvironmentRadianceMultiplier(scene ? scene->GetScene().GetLightingSettings() : SceneLightingSettings{});
}
// Rebuilds the scene environment when its calibration was edited (scene changes apply it on activation).
void Engine::RefreshSceneEnvironmentCalibration()
{
    if(!dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene())) return;
    if(ActiveEnvironmentRadianceScale()==m_appliedEnvironmentRadianceScale) return;
    std::string error;
    if(!ApplySceneEnvironment(error)) Log::Error("Scene environment calibration failed: "+error);
}
bool Engine::ApplySceneEnvironment(std::string& error)
{
    try
    {
        auto* scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
        if(!scene) return true;
        const float scale=ActiveEnvironmentRadianceScale();
        m_appliedEnvironmentRadianceScale=scale;
        const auto& reference=scene->GetScene().GetEnvironmentReference();
        if(reference == SceneDocument::NoneEnvironmentReference)
            return m_renderer.SetEnvironmentImage(nullptr,reference,"None",reference,scale);
        if(scene->IsArchiveActivation())
        {
            const auto source=scene->ArchiveSource(); const auto identity=CookedEnvironment::ArchiveIdentity(*source);
            if(Renderer::CalibratedEnvironmentIdentity(identity,scale)==m_renderer.GetEnvironmentIdentity()) return true;
            auto image=CookedEnvironment::ReadArchive(*source);
            return m_renderer.SetEnvironmentImage(image?&*image:nullptr,identity,image?"Packaged HDRI":"Neutral",reference,scale);
        }
        if(reference.empty()) return m_renderer.SetEnvironmentImage(nullptr,"neutral","Neutral",{},scale);
        return LoadProjectEnvironment(m_renderer,scene->GetProjectRoot(),m_projectAssetRegistry,reference,scale);
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
bool Engine::AssignSceneEnvironment(size_t index,std::string& error)
{
    auto* scene=dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    if(!scene)
    {
        try
        {
            const auto& entries=m_renderer.GetAvailableEnvironments();
            if(index>=entries.size()) { error="Invalid HDRI selection."; return false; }
            const auto selected=entries[index];
            const float scale=ActiveEnvironmentRadianceScale();
            if(!selected.relativePath.empty()) return m_renderer.SetEnvironment(index,scale);
            if(selected.reference == SceneDocument::NoneEnvironmentReference)
                return m_renderer.SetEnvironmentImage(nullptr,selected.reference,"None",selected.reference,scale);
            if(selected.reference.empty()) return m_renderer.SetEnvironmentImage(nullptr,"neutral","Neutral",{},scale);
            return LoadProjectEnvironment(m_renderer,GetActiveProject().GetRoot(),m_projectAssetRegistry,selected.reference,scale);
        }
        catch(const std::exception& e) { error=e.what(); return false; }
    }
    if(scene->IsArchiveActivation() || m_startup.mode!=EngineStartup::Mode::Editor) { error="Packaged HDRI assignments are immutable."; return false; }
    const auto& available=m_renderer.GetAvailableEnvironments();
    if(index>=available.size()) { error="Invalid HDRI selection."; return false; }
    const auto selected=available[index]; std::string reference=selected.reference;
    try
    {
        if(!selected.relativePath.empty())
        {
            const auto destination=scene->GetProjectRoot()/"Textures"/"Environment";
            std::filesystem::create_directories(destination);
            std::filesystem::path imported;
            if(!ProjectTextureImportService::Import(ProjectPaths(scene->GetProjectRoot()),m_projectAssetRegistry,
                WindowsWindow::GetExecutableDirectory()/"assets"/selected.relativePath,destination,imported,error)) return false;
            const auto* record=m_projectAssetRegistry.FindByPath(std::filesystem::relative(imported,scene->GetProjectRoot()));
            if(!record) throw std::runtime_error("HDRI import did not retain a stable project identity.");
            reference=record->id.ToReference();
        }
        const auto previous=scene->GetScene().GetEnvironmentReference();
        scene->GetScene().SetEnvironmentReference(reference);
        if(!ApplySceneEnvironment(error)) { scene->GetScene().SetEnvironmentReference(previous); return false; }
        return true;
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
