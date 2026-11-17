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
// The registry check runs on the calling thread (a missing or invalid assignment fails at once); the returned source reads,
// validates and hashes the file on the renderer's preparation worker (S0.5).
EnvironmentPreparationSource ProjectEnvironmentSource(const std::filesystem::path& root, const ProjectAssetRegistry& registry,
    const std::string& reference, float radianceScale)
{
    const auto id = AssetID::ParseReference(reference);
    const auto* record = id ? registry.Find(*id) : nullptr;
    const auto resolved = id ? ProjectAssetResolver::Resolve(ProjectPaths(root), registry, *id) : AssetResolution{};

    if(!record || record->type != AssetType::Texture || resolved.status != AssetAvailability::Available)
    {
        throw std::runtime_error("Required scene HDRI asset is missing or invalid.");
    }

    EnvironmentPreparationSource source;
    source.displayName = resolved.resolvedPath.stem().string();
    source.reference = reference;
    source.radianceScale = radianceScale;

    // Re-read on explicit assignment/scene replacement. No development file enters a packaged provider. The identity
    // carries the pixel hash, so an unchanged reload is recognized only after the read.
    source.load = [root, path = resolved.resolvedPath, asset = id->ToString()](std::string& identity)
    {
        auto image = CookedEnvironment::ReadLoose(path);
        const auto hash = CookedSurface::Hash(std::as_bytes(std::span(image.pixels)));
        identity = "project:" + std::filesystem::canonical(root).generic_string() + ":" + asset + ":" +
            std::to_string(image.width) + "x" + std::to_string(image.height) + ":";
        constexpr char hex[] = "0123456789abcdef";

        for(auto byte : hash)
        {
            const auto value = std::to_integer<unsigned>(byte);
            identity += hex[value >> 4];
            identity += hex[value & 15];
        }

        return image;
    };

    return source;
}

// The synchronous path, kept for scenes that are not file-backed (their HDRI selection is applied at once).
bool LoadProjectEnvironment(Renderer& renderer, const std::filesystem::path& root, const ProjectAssetRegistry& registry,
    const std::string& reference, float radianceScale)
{
    auto source = ProjectEnvironmentSource(root, registry, reference, radianceScale);
    auto image = source.load(source.identity);
    return renderer.SetEnvironmentImage(&image, source.identity, source.displayName, reference, radianceScale);
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
// S0.5: requests the active scene's environment from the renderer, which keeps rendering the published one until the
// request is prepared on the GPU. False reports only failures known before preparation (a missing or invalid assignment);
// a load or preparation failure is reported by PollSceneEnvironment. An activation request keeps its activation
// semantics (exposure snap on publication, closing the window on failure) when a calibration edit supersedes it.
bool Engine::ApplySceneEnvironment(std::string& error, bool activation)
{
    try
    {
        auto* scene = dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());

        if(!scene)
        {
            return true;
        }

        const float scale = ActiveEnvironmentRadianceScale();
        m_appliedEnvironmentRadianceScale = scale;
        const auto& reference = scene->GetScene().GetEnvironmentReference();
        EnvironmentPreparationSource source;

        if(reference == SceneDocument::NoneEnvironmentReference)
        {
            source = Renderer::NoneEnvironmentSource(scale);
        }
        else if(scene->IsArchiveActivation())
        {
            const auto archive = scene->ArchiveSource();
            const auto identity = CookedEnvironment::ArchiveIdentity(*archive);

            if(identity == "neutral")
            {
                source = Renderer::NeutralEnvironmentSource(scale);
            }
            else
            {
                source.identity = identity;
                source.displayName = "Packaged HDRI";
                source.reference = reference;
                source.radianceScale = scale;

                // The archive reader serializes its payload reads; the scene snapshot is immutable and shared.
                source.load = [archive](std::string&)
                {
                    auto image = CookedEnvironment::ReadArchive(*archive);

                    if(!image)
                    {
                        throw std::runtime_error("Missing packaged scene environment.");
                    }

                    return std::move(*image);
                };
            }
        }
        else if(reference.empty())
        {
            source = Renderer::NeutralEnvironmentSource(scale);
        }
        else
        {
            source = ProjectEnvironmentSource(scene->GetProjectRoot(), m_projectAssetRegistry, reference, scale);
        }

        m_sceneEnvironmentActivation = activation || (m_sceneEnvironmentTicket != 0 && m_sceneEnvironmentActivation);
        source.snapExposure = m_sceneEnvironmentActivation;
        m_sceneEnvironmentTicket = m_renderer.RequestEnvironment(std::move(source));
        return true;
    }
    catch(const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

// Once per frame: observes the scene environment request until it resolves. A published activation request already
// snapped exposure in its publishing frame (EnvironmentPreparationSource::snapExposure).
void Engine::PollSceneEnvironment()
{
    if(!m_sceneEnvironmentTicket)
    {
        return;
    }

    std::string error;
    const auto state = m_renderer.QueryEnvironmentPreparation(m_sceneEnvironmentTicket, &error);

    if(state == Renderer::EnvironmentPreparationState::Pending)
    {
        return;
    }

    const bool activation = std::exchange(m_sceneEnvironmentActivation, false);
    m_sceneEnvironmentTicket = 0;

    if(state != Renderer::EnvironmentPreparationState::Failed)
    {
        return;
    }

    // As before S0.5, a scene whose required environment cannot be restored closes the window.
    Log::Error(std::string(activation ? "Scene environment restore failed: " : "Scene environment update failed: ") + error);

    if(activation)
    {
        m_window.Close();
    }
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
