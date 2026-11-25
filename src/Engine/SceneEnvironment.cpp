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
// Physical sky S4: the active scene's HDRI rotation (its effective Sky group), applied when the HDRI is resampled into the sky.
float Engine::ActiveEnvironmentRotationDegrees() const
{
    const auto* scene=m_demoSceneManager.TryGetActiveScene();
    return scene ? scene->GetScene().GetLightingSettings().environmentRotationDegrees : 0.0f;
}

bool Engine::ActiveSceneUsesSkyHdri() const
{
    const auto* scene = dynamic_cast<const FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene());
    return scene && m_skyHdriRuntimeIdentity != 0 && scene->GetRuntimeIdentity() == m_skyHdriRuntimeIdentity;
}

// Rebuilds the scene environment when its calibration was edited (scene changes apply it on activation).
void Engine::RefreshSceneEnvironmentCalibration()
{
    if(!dynamic_cast<FileBackedDemoScene*>(m_demoSceneManager.TryGetActiveScene())) return;
    if(ActiveEnvironmentRadianceScale()==m_appliedEnvironmentRadianceScale &&
        ActiveEnvironmentRotationDegrees()==m_appliedEnvironmentRotationDegrees) return;
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
        const float rotation = ActiveEnvironmentRotationDegrees();
        m_appliedEnvironmentRadianceScale = scale;
        m_appliedEnvironmentRotationDegrees = rotation;
        // Physical sky S0: an Hdri sky prepares its reference; Neutral and Physical skies the neutral environment (a Physical
        // sky lights the frame through the renderer's SkyAtmosphere once it has baked; until then, and as the S0.5 environment,
        // the neutral one is shown).
        const std::string reference = scene->GetEnvironmentRequestReference();
        // W3: the standalone preview requests a changed effective sky only when it differs from this publication.
        scene->NotePublishedEnvironment(reference);
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

        // Physical sky S4, HDRI option A (§4.11): an HDRI image is resampled into the sky's persistent resources; the S0.5
        // environment then holds the neutral fallback (shown until the sky first publishes, or if the HDRI cannot be prepared).
        // builtin:none and the neutral environment stay on the S0.5 path.
        const bool skyHdri = scene->GetEffectiveSkyType() == SkyType::Hdri && !reference.empty() &&
            reference != SceneDocument::NoneEnvironmentReference && source.identity != "neutral";

        if(skyHdri && m_renderer.GetSkyAtmosphere().IsInitialized())
        {
            SkyHdriSource sky;
            sky.load = std::move(source.load);
            sky.identity = source.identity;
            sky.displayName = source.displayName;
            sky.reference = source.reference;
            sky.radianceScale = scale;
            sky.rotationDegrees = rotation;
            sky.snapExposure = source.snapExposure;
            m_renderer.RequestEnvironment(Renderer::NeutralEnvironmentSource(scale));
            m_sceneEnvironmentTicket = m_renderer.RequestSkyHdri(std::move(sky));
            m_sceneEnvironmentTicketIsSky = true;
            m_skyHdriRuntimeIdentity = scene->GetRuntimeIdentity();
            return true;
        }

        m_renderer.ClearSkyHdri();
        m_skyHdriRuntimeIdentity = 0;
        m_sceneEnvironmentTicketIsSky = false;
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
    const auto state = m_sceneEnvironmentTicketIsSky ? m_renderer.QuerySkyHdri(m_sceneEnvironmentTicket, &error) :
        m_renderer.QueryEnvironmentPreparation(m_sceneEnvironmentTicket, &error);

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
    // W3: an inherited sky belongs to the World (or the Engine defaults); it is assigned here only after enabling the Sky override.
    if(const auto* document=scene->GetSceneDocument(); document && document->kind==SceneKind::Local && !document->environmentOverrides.sky)
    {
        error="The sky is inherited. Enable the Sky override (Environment, Local Level) to assign this Local Level's own HDRI.";
        return false;
    }
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

        // Physical sky S0: choosing an HDRI (or Neutral, or None) here returns the sky type to the one the choice derives (an
        // HDRI or None: Hdri; Neutral: Neutral), so the choice is what renders. The HDRI combo stays the place to pick an HDRI
        // until Level > Sky gains its HDRI section (S4). The sky type is the document's (the Sky window's edits are), and the
        // stored physical sky is kept.
        const SceneDocument* document = scene->GetSceneDocument();
        const SceneSky before = document ? SceneSky{document->skyType, document->physicalSky} : SceneSky{};
        const SceneSky after{std::nullopt, before.physical};
        const bool skyChanged = document && !(before == after);

        if(skyChanged && !scene->SetSceneSky(before, after, &error))
        {
            scene->GetScene().SetEnvironmentReference(previous);
            return false;
        }

        if(!ApplySceneEnvironment(error))
        {
            scene->GetScene().SetEnvironmentReference(previous);

            if(skyChanged)
            {
                std::string restoreError;
                scene->SetSceneSky(after, before, &restoreError);
            }

            return false;
        }

        return true;
    }
    catch(const std::exception& e) { error=e.what(); return false; }
}
