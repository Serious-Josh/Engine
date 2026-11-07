#include "Engine/Engine.h"
#include "assets/AcousticMaterial.h"
#include "assets/ArchiveCookedMeshProvider.h"
#include "assets/CookedMesh.h"

#include <map>
#include <memory>
#include <stdexcept>

bool Engine::StartPackagedGame(FileSceneRuntime& runtime, std::string& error)
{
    if(m_startup.mode != EngineStartup::Mode::PackagedRuntime || !runtime.IsArchiveActivation())
    {
        error = "Packaged Game Play requires an archive-only runtime scene.";
        return false;
    }

    // Packaged games compile the source closures Build Game cooked into the archive; the
    // include policy and closure revision are rechecked exactly as for project sources.
    const auto scripts = [&runtime](Scripting::ScriptHost& host, const AssetID& asset, Scripting::ModuleID& module)
    {
        try
        {
            const auto& archive = runtime.GetArchiveScene();

            if(!archive || !archive->provider)
            {
                throw std::runtime_error("The packaged scene has no archive.");
            }

            const auto bytes = archive->provider->ReadEntry(asset, EngineAssetPak::Type::Resource,
                Scripting::PackagedSnapshotVersion, Scripting::PackagedSnapshotBytes);
            return host.BuildSnapshot(Scripting::DecodeSnapshot(bytes), module);
        }
        catch(const std::exception& exception)
        {
            Scripting::Result result;
            result.diagnostics.push_back({{}, 0, 0, "Error",
                "Packaged script " + asset.ToReference() + " is unavailable: " + exception.what()});
            return result;
        }
    };

    // The acoustic scene comes from the archive alone: cooked meshes decoded once, built-in materials, and the project materials Build Game cooked.
    GameRuntime::AcousticAssets acousticAssets;
    const auto decodedMeshes = std::make_shared<std::map<std::string, std::shared_ptr<const ImportedMesh>>>();
    acousticAssets.meshGeometryByReference = [&runtime, decodedMeshes](const std::string& reference) -> const ImportedMesh*
    {
        const auto found = decodedMeshes->find(reference);

        if(found != decodedMeshes->end())
        {
            return found->second.get();
        }

        const auto id = AssetID::ParseReference(reference);
        const auto& archive = runtime.GetArchiveScene();
        const EngineAssetPak::Entry* const entry = id && archive && archive->provider ? archive->provider->ArchiveEntry(*id) : nullptr;

        if(!entry)
        {
            return nullptr;
        }

        const CookedMesh::Policy policy;
        const auto bytes = archive->provider->ReadEntry(*id, entry->type, entry->payloadVersion, policy.maxDecodedBytes);
        auto decoded = CookedMesh::Decode(bytes, policy).geometry;
        const ImportedMesh* const geometry = decoded.get();
        decodedMeshes->emplace(reference, std::move(decoded));
        return geometry;
    };
    acousticAssets.material = [&runtime](const std::string& reference, std::string* materialError) -> std::optional<AcousticMaterialData>
    {
        if(reference.starts_with(AcousticMaterials::BuiltinPrefix))
        {
            return AcousticMaterials::Resolve(reference, nullptr, nullptr, materialError);
        }

        const auto id = AssetID::ParseReference(reference);
        const auto& archive = runtime.GetArchiveScene();

        if(!id || !archive || !archive->provider || !archive->provider->ArchiveEntry(*id))
        {
            if(materialError)
            {
                *materialError = "the acoustic material is not in the game archive";
            }

            return std::nullopt;
        }

        const auto bytes = archive->provider->ReadEntry(*id, EngineAssetPak::Type::Resource, AcousticMaterials::PackagedPayloadVersion,
            AcousticMaterials::MaximumPackagedBytes);
        AcousticMaterialData material;

        if(!AcousticMaterials::DecodePackaged(bytes, material, materialError))
        {
            return std::nullopt;
        }

        return material;
    };
    acousticAssets.probeBatch = [&runtime](const std::string& reference, std::string* probeError) -> std::optional<ProbeBatchAssets::Data>
    {
        const auto id = AssetID::ParseReference(reference);
        const auto& archive = runtime.GetArchiveScene();

        if(!id || !archive || !archive->provider || !archive->provider->ArchiveEntry(*id))
        {
            if(probeError)
            {
                *probeError = "the probe batch is not in the game archive";
            }

            return std::nullopt;
        }

        const auto bytes = archive->provider->ReadEntry(*id, EngineAssetPak::Type::Resource, ProbeBatchAssets::PackagedPayloadVersion, ProbeBatchAssets::MaximumBytes);
        ProbeBatchAssets::Data data;

        if(!ProbeBatchAssets::Decode(bytes, data, probeError))
        {
            return std::nullopt;
        }

        return data;
    };
    m_gameRuntime.SetAcousticAssets(std::move(acousticAssets));

    return m_gameRuntime.Start(runtime, m_renderer, m_packagedMainCamera, m_window.GetWidth(), m_window.GetHeight(),
        error, scripts);
}

bool Engine::AdvancePackagedGame(FileSceneRuntime& runtime, double elapsedSeconds, std::string& error)
{
    if(m_startup.mode != EngineStartup::Mode::PackagedRuntime || !runtime.IsArchiveActivation())
    {
        error = "Packaged Game Play lost its archive scene.";
        return false;
    }

    // The packaged window always owns gameplay input, except while a released cursor lock waits
    // for the click that recaptures it.
    const WindowsWindow::CursorRectangle client{0, 0, static_cast<int>(m_window.GetWidth()),
        static_cast<int>(m_window.GetHeight())};
    const bool captured = UpdateGameCursor(client, m_input.WasMouseButtonPressed(MouseButton::Left));
    const bool suppressed = m_gameRuntime.WantsCursorLock() && !captured;

    if(!m_gameRuntime.Update(runtime, m_renderer, {elapsedSeconds, suppressed ? nullptr : &m_input, captured}, error))
    {
        UpdateGameCursor(std::nullopt, false);
        return false;
    }

    return true;
}

bool Engine::UpdateGameCursor(const std::optional<WindowsWindow::CursorRectangle>& area, bool areaClicked)
{
    if(!m_gameRuntime.IsActive())
    {
        m_gameCursorReleased = false;
        m_window.SetCursorConfinement(std::nullopt);
        return false;
    }

    if(m_window.IsCursorConfined() && m_input.WasKeyPressed(KeyCode::Home))
    {
        m_gameCursorReleased = true;
    }
    else if(area && areaClicked)
    {
        m_gameCursorReleased = false;
    }

    const bool wanted = area && m_gameRuntime.WantsCursorLock() && !m_gameCursorReleased;
    // Applied every frame so window moves and viewport resizes keep the confinement exact.
    m_window.SetCursorConfinement(wanted ? area : std::nullopt);
    return wanted && m_window.IsCursorConfined();
}

bool Engine::SubmitCharacterMovement(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, Vector3 velocity, std::string& error)
{
    return m_physicsSimulation.SubmitCharacterMovement(sessionGeneration, objectID, fixedStep, velocity, error);
}

bool Engine::SubmitCharacterJump(std::uint64_t sessionGeneration, std::string_view objectID,
    std::uint64_t fixedStep, std::string& error)
{
    return m_physicsSimulation.SubmitCharacterJump(sessionGeneration, objectID, fixedStep, error);
}
