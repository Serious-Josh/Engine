#include "assets/AcousticMaterial.h"
#include "assets/ProbeBatchAsset.h"
#include "Engine/BuildGame.h"
#include "Engine/PackagedAudioBanks.h"
#include "Engine/PackagedBootstrap.h"
#include "Engine/PackagedRenderingSettings.h"
#include "assets/CookedScene.h"
#include "Scene/RenderView.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ProjectScriptAssets.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Core/Log.h"

#include <Windows.h>
#include "Platform/Windows/WindowsText.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace BuildGame
{
namespace fs = std::filesystem;

namespace
{
std::string UTF8Path(const fs::path& path)
{
    const auto utf8Path = path.u8string();
    return {utf8Path.begin(), utf8Path.end()};
}

void Require(bool condition, const std::string& error)
{
    if (!condition)
    {
        throw std::runtime_error(error);
    }
}

// The Agility SDK runtime is the only deployment content outside the top level: D3D12SDKPath names
// this one fixed directory, and only these files are ever owned inside it.
constexpr std::string_view agilityRuntimeDirectory = "D3D12";
constexpr std::string_view agilityRuntimeFiles[] = {"D3D12Core.dll"};

std::string AgilityRuntimeEntry(std::string_view fileName)
{
    return std::string(agilityRuntimeDirectory) + "/" + std::string(fileName);
}

bool IsAgilityRuntimeEntry(std::string_view manifestEntry)
{
    return std::ranges::any_of(agilityRuntimeFiles,
        [&](std::string_view fileName)
        {
            return manifestEntry == AgilityRuntimeEntry(fileName);
        });
}

// Symlinks, junctions and other reparse points are never owned deployment content.
bool IsReparsePoint(const fs::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

struct Pin
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    explicit Pin(const fs::path& path)
    {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        Require(handle != INVALID_HANDLE_VALUE,
            "Cannot pin build input (missing, locked or permission denied): " + path.string());

        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Build inputs must be ordinary files: " + path.string());
        }
    }

    ~Pin()
    {
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), text.size());
    output.close();

    Require(bool(output), "Cannot write deployment file: " + path.string());
}

void CheckDestination(const fs::path& outputDirectory)
{
    if (!fs::exists(outputDirectory))
    {
        return;
    }

    Require(fs::is_directory(outputDirectory) && !fs::is_symlink(outputDirectory),
        "Output must be an ordinary directory.");

    if (fs::is_empty(outputDirectory))
    {
        return;
    }

    const auto manifestPath = outputDirectory / "BuildGame.files";
    Require(fs::exists(manifestPath) && fs::file_size(manifestPath) < 64 * 1024,
        "Output is nonempty and is not an owned Build Game deployment. Choose an empty/new directory.");

    std::ifstream manifest(manifestPath);
    std::string fileName;
    std::set<std::string> ownedFiles{"BuildGame.files"};
    std::set<std::string> ownedRuntimeFiles;
    while (std::getline(manifest, fileName))
    {
        if (IsAgilityRuntimeEntry(fileName))
        {
            ownedRuntimeFiles.insert(fileName);
            continue;
        }

        Require(fs::u8path(fileName).filename() == fs::u8path(fileName) && !fileName.empty(),
            "Invalid deployment ownership list.");
        ownedFiles.insert(fileName);
    }

    const auto unrelatedContent = "Output contains unrelated files; choose a new directory. Nothing was overwritten.";

    for (const auto& entry : fs::directory_iterator(outputDirectory))
    {
        // The owned Agility runtime directory holds only owned, ordinary allowlisted files.
        if (!ownedRuntimeFiles.empty() && entry.path().filename() == fs::u8path(agilityRuntimeDirectory))
        {
            Require(entry.is_directory() && !IsReparsePoint(entry.path()), unrelatedContent);

            for (const auto& runtimeEntry : fs::directory_iterator(entry.path()))
            {
                Require(runtimeEntry.is_regular_file() && !runtimeEntry.is_symlink() &&
                    !IsReparsePoint(runtimeEntry.path()) &&
                    ownedRuntimeFiles.contains(AgilityRuntimeEntry(UTF8Path(runtimeEntry.path().filename()))),
                    unrelatedContent);
            }

            continue;
        }

        // Keep filename conversion behind the existing ordinary-file checks.
        Require(entry.is_regular_file() && !entry.is_symlink() &&
            ownedFiles.contains(UTF8Path(entry.path().filename())),
            unrelatedContent);
    }

    for (const auto& name : ownedFiles)
    {
        if (fs::u8path(name).extension() == ".exe" && fs::exists(outputDirectory / fs::u8path(name)))
        {
            HANDLE executableHandle = CreateFileW((outputDirectory / fs::u8path(name)).c_str(),
                GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            Require(executableHandle != INVALID_HANDLE_VALUE,
                "Previous game executable is locked or not writable. Close it before rebuilding.");
            CloseHandle(executableHandle);
        }
    }
}

// References point only into the shared Service state retained by the worker capture.
// A concrete reporter avoids allocating a type-erased callback at each stage boundary.
struct BuildProgress
{
    std::mutex& mutex;
    const bool& cancelled;
    Status& status;

    void Report(float value, const std::string& message) const
    {
        std::lock_guard lock(mutex);
        Require(!cancelled, "Build canceled; previous output preserved.");
        status.progress = value;
        status.message = message;
    }
};

// These are the existing worker-local payloads and source pins, grouped for stage calls.
// Construct inside the worker try scope so pins still release before failure cleanup.
struct BuildAssets
{
    std::vector<CookedSurface::Asset> cookedAssets;
    std::vector<std::unique_ptr<Pin>> sourcePins;
    std::uint64_t retainedPayloadBytes = 0;

    void Append(CookedSurface::Asset asset)
    {
        Require(asset.bytes.size() <= 512ull * 1024 * 1024 - retainedPayloadBytes,
            "Build payloads exceed 512 MiB batch limit.");
        retainedPayloadBytes += asset.bytes.size();
        cookedAssets.push_back(std::move(asset));
    }
};

fs::path PrepareOutputDirectory(const Request& request)
{
    auto outputDirectory = fs::absolute(request.settings.output).lexically_normal();
    Require(outputDirectory.has_filename() && outputDirectory != outputDirectory.root_path(),
        "Choose a named output directory, not a drive root.");

    // Never stage/publish over project/source trees, including an ancestor of the project.
    const auto projectDirectory = fs::weakly_canonical(request.project);
    const auto canonicalOutputDirectory = fs::weakly_canonical(outputDirectory);
    const auto isWithinDirectory = [](const fs::path& child, const fs::path& parent)
    {
        const auto relativePath = child.lexically_relative(parent);
        return !relativePath.empty() && *relativePath.begin() != "..";
    };
    Require(!isWithinDirectory(projectDirectory, canonicalOutputDirectory) &&
        !isWithinDirectory(canonicalOutputDirectory, projectDirectory),
        "Output cannot be inside or contain the development project.");
    CheckDestination(outputDirectory);
    fs::create_directories(outputDirectory.parent_path());
    return outputDirectory;
}

// With Steam Audio on, every project material a scene names, so the game never needs the project's files. A built-in travels in the code. A material
// that cannot be read refuses the build, because the game would otherwise simulate a different surface than was authored.
void CookAcousticMaterials(const Request& request, BuildAssets& build)
{
    if(!request.projectSettings.audio.steamAudio)
    {
        return;
    }

    const ProjectPaths paths(request.project);
    std::set<std::string> references;

    for(const auto& object : request.document.objects)
    {
        if(object.acoustics && object.acoustics->enabled)
        {
            for(const auto& [submesh, reference] : object.acoustics->submeshMaterials)
            {
                if(!reference.starts_with(AcousticMaterials::BuiltinPrefix))
                {
                    references.insert(reference);
                }
            }
        }
    }

    for(const std::string& reference : references)
    {
        std::string error;
        const auto material = AcousticMaterials::Resolve(reference, &paths, &request.registry, &error);
        Require(material.has_value(), "Acoustic material " + reference + " cannot be packaged: " + error);
        build.Append({*AssetID::ParseReference(reference), EngineAssetPak::Type::Resource, AcousticMaterials::PackagedPayloadVersion,
            AcousticMaterials::EncodePackaged(*material)});
    }
}

// M7: staleness is reported at Play. A file that is present but fails validation stops the build, because it would ship broken.
void CookProbeBatches(const Request& request, BuildAssets& build)
{
    if(!request.projectSettings.audio.steamAudio)
    {
        return;
    }

    const ProjectPaths paths(request.project);
    std::set<std::string> references;

    for(const auto& object : request.document.objects)
    {
        if(object.probeVolume && object.probeVolume->enabled && !object.probeVolume->probeBatch.empty())
        {
            references.insert(object.probeVolume->probeBatch);
        }
    }

    for(const std::string& reference : references)
    {
        std::string error;
        const auto data = ProbeBatchAssets::Resolve(reference, paths, request.registry, &error);
        Require(data.has_value(), "Probe batch " + reference + " cannot be packaged: " + error);
        build.Append({*AssetID::ParseReference(reference), EngineAssetPak::Type::Resource, ProbeBatchAssets::PackagedPayloadVersion,
            ProbeBatchAssets::Encode(*data)});
    }
}

void CookMeshAssets(const Request& request, BuildAssets& build, const BuildProgress& progress)
{
    std::set<std::string> meshIDs;
    for (const auto& object : request.document.objects)
    {
        if (!object.meshReference.empty())
        {
            auto assetID = AssetID::ParseReference(object.meshReference);
            Require(bool(assetID),
                "Only registered mesh assets can be packaged; procedural/builtin mesh references are unsupported.");
            meshIDs.insert(assetID->ToString());
        }
    }
    for (const auto& meshIDText : meshIDs)
    {
        progress.Report(.1f, "Cooking mesh " + meshIDText);
        const auto assetID = *AssetID::Parse(meshIDText);
        const auto* assetRecord = request.registry.Find(assetID);
        const auto resolvedAsset = ProjectAssetResolver::Resolve(
            ProjectPaths(request.project), request.registry, assetID);
        Require(assetRecord && assetRecord->type == AssetType::Mesh &&
            resolvedAsset.status == AssetAvailability::Available,
            "Referenced mesh is missing or invalid: " + meshIDText);
        Require(fs::file_size(resolvedAsset.resolvedPath) <= 64ull * 1024 * 1024,
            "Mesh source exceeds 64 MiB build limit.");
        build.sourcePins.push_back(std::make_unique<Pin>(resolvedAsset.resolvedPath));
        auto decodedGeometry = ModelImporter::LoadFlattened(resolvedAsset.resolvedPath,
            MeshUVOrientation::FlipV, CookedMesh::Policy{}.maxDecodedBytes, assetRecord->meshImportUnits);
        auto encodedMesh = CookedMesh::Encode(decodedGeometry, CookedMesh::Policy{});
        Require(encodedMesh.status == CookedMesh::Status::Success, std::string(encodedMesh.message));
        build.Append({assetID, EngineAssetPak::Type::Mesh, 1, std::move(encodedMesh.payload)});
    }
}

void CookSceneAssets(const Request& request, const AssetID& sceneAssetID,
    BuildAssets& build, const BuildProgress& progress)
{
    progress.Report(.35f, "Cooking scene metadata, materials and textures...");
    std::vector<CookedScene::MeshInput> meshInputs;
    for (const auto& asset : build.cookedAssets)
    {
        meshInputs.push_back({asset.id, asset.generation, asset.bytes});
    }
    std::optional<CookedScene::EnvironmentInput> environmentInput;
    if (!request.document.environmentReference.empty() && request.document.environmentReference != SceneDocument::NoneEnvironmentReference)
    {
        progress.Report(.3f, "Cooking assigned HDR environment...");
        const auto assetID = *AssetID::ParseReference(request.document.environmentReference);
        const auto* assetRecord = request.registry.Find(assetID);
        const auto resolvedAsset = ProjectAssetResolver::Resolve(
            ProjectPaths(request.project), request.registry, assetID);
        Require(assetRecord && assetRecord->type == AssetType::Texture &&
            resolvedAsset.status == AssetAvailability::Available,
            "Required scene HDR environment is missing or invalid.");
        auto environmentPayload = CookedEnvironment::Cook(resolvedAsset.resolvedPath, assetID, 1);
        build.Append({assetID, EngineAssetPak::Type::Resource, 1, std::move(environmentPayload)});
        environmentInput = CookedScene::EnvironmentInput{assetID, 1, build.cookedAssets.back().bytes};
    }
    auto scenePayload = CookedScene::Cook(request.document, meshInputs, request.registry, {},
        environmentInput ? &*environmentInput : nullptr);
    auto surfaceAssets = CookedSurface::CookDependencies(
        CookedScene::Decode(scenePayload), ProjectPaths(request.project), request.registry);
    for (auto& asset : surfaceAssets)
    {
        build.Append(std::move(asset));
    }
    build.Append({sceneAssetID, EngineAssetPak::Type::Scene, 1, std::move(scenePayload)});
}

// AS.5: every Script asset an enabled component uses is validated exactly as Play validates it
// and packaged as its immutable source closure. Invalid scripts or classes fail the build.
void CookScriptAssets(const Request& request, BuildAssets& build, const BuildProgress& progress)
{
    static_assert(Scripting::PackagedSnapshotVersion == 1, "Non-scene archive payloads are written as version 1.");
    std::map<std::string, std::set<std::string>> classesByScript;

    for(const auto& object : request.document.objects)
    {
        if(!object.script || !object.script->enabled)
        {
            continue;
        }

        Require(AssetID::ParseReference(object.script->scriptReference).has_value(),
            "Object '" + object.name + "' has a Script component without a script asset.");
        Require(!object.script->className.empty(),
            "Object '" + object.name + "' has a Script component without a selected class.");
        classesByScript[object.script->scriptReference].insert(object.script->className);
    }

    if(classesByScript.empty())
    {
        return;
    }

    progress.Report(.4f, "Validating and packaging scripts...");
    Scripting::ScriptHost host;
    Require(host.Initialize().status == Scripting::Status::Ready, "Cannot initialize script validation.");

    for(const auto& [reference, classes] : classesByScript)
    {
        const auto assetID = *AssetID::ParseReference(reference);
        const auto result = ProjectScriptAssets::Validate(host, ProjectPaths(request.project), request.registry, assetID);
        const std::string asset = result.snapshot ? result.snapshot->rootSection : reference;

        if(result.status != Scripting::Status::Compiled || !result.snapshot)
        {
            std::string detail = "unknown error";

            if(!result.diagnostics.empty())
            {
                // The first error, not AngelScript's preceding informational "Compiling ..." line.
                const auto error = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const Scripting::Diagnostic& diagnostic) { return diagnostic.severity == "Error"; });
                const auto& first = error != result.diagnostics.end() ? *error : result.diagnostics.front();
                detail = first.section + "(" + std::to_string(first.line) + "," + std::to_string(first.column) + "): " +
                    first.text;
            }

            Require(false, "Script " + asset + " failed to compile: " + detail);
        }

        for(const auto& className : classes)
        {
            Require(std::find(result.classes.begin(), result.classes.end(), className) != result.classes.end(),
                "Script " + asset + " defines no EAScript class '" + className + "'.");
        }

        build.Append({assetID, EngineAssetPak::Type::Resource, 1, Scripting::EncodeSnapshot(*result.snapshot)});
    }
}

// Reads a whole pinned build input through its pinned handle, so the cooked bytes are exactly the pinned file's.
std::vector<std::byte> ReadPinned(const Pin& pin, const fs::path& path, std::size_t limit, const std::string& what)
{
    LARGE_INTEGER size{};
    Require(GetFileSizeEx(pin.handle, &size) && size.QuadPart >= 0, "Cannot read the size of " + what + ".");
    Require(static_cast<unsigned long long>(size.QuadPart) <= limit,
        what + " is " + std::to_string(size.QuadPart) + " bytes; the limit is " + std::to_string(limit) +
        " bytes. Split it into smaller banks in FMOD Studio.");

    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    Require(ReadFile(pin.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size(),
        "Cannot read " + what + ": " + path.string());
    return bytes;
}

// Audio.1D: every available AudioBank of the project is packaged unchanged as its own Resource entry, plus the bank
// list entry naming them in load order. Registered banks whose file is gone (identities FMOD Studio renames leave
// behind, Audio.1B) are skipped with a warning; an available bank that cannot be read, is too large or is not an FMOD
// Studio bank fails the build.
void CookAudioBanks(const Request& request, BuildAssets& build, const BuildProgress& progress)
{
    const std::vector<AssetRecord> registered = PackagedAudioBanks::LoadOrder(request.registry.GetAssets(AssetType::AudioBank));

    if(registered.empty())
    {
        return;
    }

    progress.Report(.5f, "Packaging FMOD Studio banks...");
    const ProjectPaths paths(request.project);
    std::vector<AssetRecord> packaged;
    std::uint64_t packagedBytes = 0;

    for(const AssetRecord& bank : registered)
    {
        const std::string name = bank.relativePath.generic_string();
        Require(bank.id != PackagedAudioBanks::ListID() && std::none_of(build.cookedAssets.begin(),
            build.cookedAssets.end(), [&](const CookedSurface::Asset& asset) { return asset.id == bank.id; }),
            "Audio bank " + name + " collides with another packaged asset identity.");

        const auto resolution = ProjectAssetResolver::Resolve(paths, request.registry, bank.id);

        if(resolution.status == AssetAvailability::Missing)
        {
            Log::Warning("Build Game: audio bank " + name + " is registered but missing; it is not packaged "
                "(rebuild it in FMOD Studio if the game needs it).");
            continue;
        }

        Require(resolution.status == AssetAvailability::Available,
            "Audio bank " + name + " is unreadable or outside Content/Audio/Banks; fix or remove it.");

        const Pin pin(resolution.resolvedPath);
        const std::string what = "audio bank " + name;
        std::vector<std::byte> bytes = ReadPinned(pin, resolution.resolvedPath, PackagedAudioBanks::MaximumBankBytes, what);
        std::string error;
        const bool isBank = PackagedAudioBanks::IsBank(bytes, error);
        Require(isBank, "Audio bank " + name + " is invalid: " + error + ". Rebuild the banks in FMOD Studio.");

        packagedBytes += bytes.size();
        build.Append({bank.id, EngineAssetPak::Type::Resource, 1, std::move(bytes)});
        packaged.push_back(bank);
    }

    if(packaged.empty())
    {
        return;
    }

    build.Append({PackagedAudioBanks::ListID(), EngineAssetPak::Type::Resource, 1,
        PackagedAudioBanks::EncodeList(packaged)});
    Log::Info("Build Game: packaged " + std::to_string(packaged.size()) + " FMOD Studio banks (" +
        std::to_string(packagedBytes) + " bytes).");
}

void CompilePackagedShaders(const Request& request, BuildAssets& build, const BuildProgress& progress)
{
    progress.Report(.55f, "Compiling packaged shaders offline...");
    // PS.5B: the renderer's packaged inventory; CookBootstrap enforces the bootstrap limits.
    ShaderBootstrapStatistics statistics;
    auto shaderPayload = ShaderLibrary::CookBootstrap(request.shaderRoot, GetPackagedShaderInventory(),
        GetPackagedLibraryInventory(), &statistics);
    Log::Info("Packaged shaders: " + std::to_string(statistics.shaders) + " graphics/compute and " +
        std::to_string(statistics.libraries) + " DXR libraries (" + std::to_string(statistics.optionalLibraries) +
        " optional RayQuery variants; " + std::to_string(statistics.libraryCacheHits) + " from the DXIL cache), " +
        std::to_string(statistics.bytes) + " bytes, largest record " + std::to_string(statistics.largestRecordBytes) +
        " bytes, cooked in " + std::to_string(statistics.seconds) + " s.");
    build.Append({PackagedBootstrap::ShaderID(), EngineAssetPak::Type::Shader, 1, std::move(shaderPayload)});

    // The path tracer's material tables travel verbatim; their SHA-256 pin is checked here and again at startup.
    auto materialTables = PackagedBootstrap::ReadBundledMaterialTables(request.shaderRoot);
    build.Append({PackagedBootstrap::MaterialTablesID(), EngineAssetPak::Type::Resource,
        PackagedBootstrap::MaterialTablesPayloadVersion, std::move(materialTables)});

    // PS.4: the saved Project Settings, checked to decode back to exactly what was cooked.
    const AssetID settingsID = PackagedRenderingSettings::ID();
    Require(std::none_of(build.cookedAssets.begin(), build.cookedAssets.end(),
        [&](const CookedSurface::Asset& asset) { return asset.id == settingsID; }),
        "Asset " + settingsID.ToString() + " collides with the reserved packaged rendering settings identity.");
    auto settingsPayload = PackagedRenderingSettings::Encode(request.projectSettings);
    const auto decoded = PackagedRenderingSettings::Decode(settingsPayload);
    Require(decoded.source == PackagedRenderingSettings::Source::Cooked &&
        decoded.rendering == request.projectSettings.rendering && decoded.audio == request.projectSettings.audio,
        "Packaged rendering settings do not round-trip; the saved Project Settings may need to be saved again.");
    build.Append({settingsID, EngineAssetPak::Type::Resource, 1, std::move(settingsPayload)});
    std::uint64_t totalPayloadBytes = 0;
    for (const auto& asset : build.cookedAssets)
    {
        totalPayloadBytes += asset.bytes.size();
        Require(totalPayloadBytes <= 512ull * 1024 * 1024,
            "Build payloads exceed 512 MiB batch limit.");
    }
}

void WriteAndVerifyArchives(const Request& request, const AssetID& sceneAssetID,
    const fs::path& stagingDirectory, const BuildAssets& build, const BuildProgress& progress)
{
    progress.Report(.75f, "Writing and validating matching game archives...");
    std::vector<EngineAssetPak::PreparedAsset> archiveAssets;
    for (const auto& asset : build.cookedAssets)
    {
        const auto payloadVersion = asset.type == EngineAssetPak::Type::Scene ? CookedScene::Version : 1;
        archiveAssets.push_back({asset.id, asset.type, payloadVersion, asset.generation, asset.bytes, 16});
    }
    EngineAssetPak::Write(stagingDirectory / "Game.toc", stagingDirectory / "Game.eap", archiveAssets);
    {
        auto archiveReader = EngineAssetPak::Reader::Open(
            stagingDirectory / "Game.toc", stagingDirectory / "Game.eap");
        for (const auto& entry : archiveReader.Entries())
        {
            archiveReader.Read(entry.id);
        }

        // Admitted exactly as packaged preflight admits it.
        const auto settings = PackagedRenderingSettings::Admit(archiveReader.Lookup(PackagedRenderingSettings::ID()),
            [&]() { return archiveReader.Read(PackagedRenderingSettings::ID()); });
        Require(settings.source == PackagedRenderingSettings::Source::Cooked &&
            settings.rendering == request.projectSettings.rendering,
            "Packaged rendering settings differ from the saved Project Settings.");

        // Audio.1D: the bank list decodes, and every bank it names is in the archive and still a bank.
        if(archiveReader.Lookup(PackagedAudioBanks::ListID()))
        {
            for(const AssetRecord& bank : PackagedAudioBanks::DecodeList(archiveReader.Read(PackagedAudioBanks::ListID())))
            {
                const auto* entry = archiveReader.Lookup(bank.id);
                Require(entry && entry->type == EngineAssetPak::Type::Resource,
                    "Packaged audio bank " + bank.relativePath.generic_string() + " is missing from the archive.");
                std::string error;
                const bool isBank = PackagedAudioBanks::IsBank(archiveReader.Read(bank.id), error);
                Require(isBank, "Packaged audio bank " + bank.relativePath.generic_string() + " is invalid: " + error);
            }
        }
    }
    {
        auto verifiedScene = CookedScene::OpenArchive(
            stagingDirectory / "Game.toc", stagingDirectory / "Game.eap", sceneAssetID);
        Require(verifiedScene->metadata.document.id == request.document.id,
            "Packaged startup scene identity differs.");
    }
}

void DeployRuntimeFiles(const Request& request, const AssetID& sceneAssetID,
    const fs::path& stagingDirectory, const BuildProgress& progress)
{
    progress.Report(.9f, "Assembling standalone executable and runtime dependencies...");
    std::vector<std::string> deploymentFiles{
        "Game.toc", "Game.eap", "Game.startup", request.settings.title + ".exe"};
    const auto copyRuntimeFile = [&](const fs::path& source, const std::string& fileName)
    {
        Pin pin(source);
        fs::copy_file(source, stagingDirectory / fs::u8path(fileName));
    };
    copyRuntimeFile(request.runtimeDirectory / "EngineRuntime.exe", request.settings.title + ".exe");
    copyRuntimeFile(request.runtimeDirectory / "AngelScriptNotices.txt", "AngelScriptNotices.txt");
    std::set<std::string> requiredDLLs{"NRD.dll", "NRI.dll", "dxcompiler.dll", "dxil.dll"};
    unsigned int assimpDLLCount = 0;
    unsigned int textureDLLCount = 0;
    for (const auto& entry : fs::directory_iterator(request.runtimeDirectory))
    {
        const auto fileName = entry.path().filename().string();
        if (entry.path().extension() == ".dll" &&
            (fileName == "NRD.dll" || fileName == "NRI.dll" || fileName == "dxcompiler.dll" ||
             fileName == "dxil.dll" || fileName.starts_with("assimp-") || fileName.starts_with("DirectXTex")))
        {
            copyRuntimeFile(entry.path(), fileName);
            deploymentFiles.push_back(fileName);
            requiredDLLs.erase(fileName);
            if (fileName.starts_with("assimp-"))
            {
                ++assimpDLLCount;
            }
            if (fileName.starts_with("DirectXTex"))
            {
                ++textureDLLCount;
            }
        }
    }
    Require(deploymentFiles.size() == 10 && requiredDLLs.empty() &&
        assimpDLLCount == 1 && textureDLLCount == 1,
        "Runtime directory is missing required DLLs or has ambiguous dependency versions; build EngineRuntime first.");

    // dxcompiler.dll and dxil.dll never ship without the license texts of the DXC package they came from.
    for(const auto* fileName : {"DXC-LICENSE-MS.txt", "DXC-LICENSE-MIT.txt", "DXC-LICENSE-LLVM.txt"})
    {
        Require(fs::is_regular_file(request.runtimeDirectory / fileName),
            std::string("Runtime directory is missing the DirectX Shader Compiler notice ") + fileName +
            "; build EngineRuntime first.");

        copyRuntimeFile(request.runtimeDirectory / fileName, fileName);
        deploymentFiles.emplace_back(fileName);
    }

    // The executable's D3D12SDKPath requires the Agility runtime in D3D12/ beside it. Only the
    // allowlisted redistributable is deployed; debug layers and SDK tools are never shipped.
    const fs::path runtimeSourceDirectory = request.runtimeDirectory / fs::u8path(agilityRuntimeDirectory);
    Require(fs::is_directory(runtimeSourceDirectory) && !IsReparsePoint(runtimeSourceDirectory),
        "Runtime directory is missing the D3D12 Agility SDK runtime folder; build EngineRuntime first.");
    Require(fs::create_directory(stagingDirectory / fs::u8path(agilityRuntimeDirectory)),
        "Cannot create the staged D3D12 runtime folder.");

    for (const std::string_view runtimeFile : agilityRuntimeFiles)
    {
        const fs::path runtimeSource = runtimeSourceDirectory / fs::u8path(runtimeFile);
        Require(fs::is_regular_file(runtimeSource),
            "Runtime directory is missing D3D12/" + std::string(runtimeFile) + "; build EngineRuntime first.");

        const std::string runtimeEntry = AgilityRuntimeEntry(runtimeFile);
        copyRuntimeFile(runtimeSource, runtimeEntry);
        deploymentFiles.push_back(runtimeEntry);
    }

    deploymentFiles.emplace_back("AngelScriptNotices.txt");

    // PS.5E: DLSS Super Resolution's runtime, only for saved settings that can use it (Path Tracing with a denoiser;
    // without one DLSS is never eligible). NGX loads nvngx_dlss.dll from beside the executable. The runtime directory
    // holds the SDK's production lib/Windows_x86_64/rel copy (engine_deploy_dlss(EngineRuntime)); the watermarked
    // development DLL is never deployed there. A game that requests DLSS is never built without its runtime.
    const RenderingSettings& rendering = request.projectSettings.rendering;
    const bool dlssUsable = rendering.lightingMode == ProjectLightingMode::PathTracing && rendering.pathTracing.dlss &&
        rendering.pathTracing.denoiser != ProjectPathTraceDenoiser::Off;

    if(dlssUsable)
    {
        const fs::path dlssRuntime = request.runtimeDirectory / "nvngx_dlss.dll";
        Require(fs::is_regular_file(dlssRuntime) && !IsReparsePoint(dlssRuntime),
            "Project Settings enable DLSS, but the runtime directory has no nvngx_dlss.dll (EngineRuntime was built "
            "without the NVIDIA DLSS SDK). Turn DLSS off in Project Settings, or build EngineRuntime with the SDK.");

        copyRuntimeFile(dlssRuntime, "nvngx_dlss.dll");
        deploymentFiles.emplace_back("nvngx_dlss.dll");
        // NVIDIA RTX supplement 7.1(b) and 4 (../DLSS/LICENSE.txt) bind the game's author, not the build.
        Log::Info("Build Game: deployed NVIDIA DLSS Super Resolution (nvngx_dlss.dll). Its license requires the game "
            "to attribute the DLSS SDK with the NVIDIA Marks (splash screen, about box, credits) and NVIDIA to be "
            "notified before commercial release.");
    }

    // Audio.1D: FMOD's production runtime (EngineRuntime delay-loads fmod.dll and fmodstudio.dll in every
    // configuration; the Editor's Debug logging variants fmodL/fmodstudioL are never deployed). Both or neither: a
    // runtime directory with one of them is inconsistent. A project with audio banks needs them.
    const bool fmodCore = fs::is_regular_file(request.runtimeDirectory / "fmod.dll");
    const bool fmodStudio = fs::is_regular_file(request.runtimeDirectory / "fmodstudio.dll");
    const bool projectHasBanks = !request.registry.GetAssets(AssetType::AudioBank).empty();
    Require(fmodCore == fmodStudio, "The runtime directory has only one of fmod.dll and fmodstudio.dll; rebuild "
        "EngineRuntime.");
    Require(fmodCore || !projectHasBanks, "The project has FMOD Studio banks, but the runtime directory has no FMOD "
        "runtime (EngineRuntime was built with ENGINE_ENABLE_FMOD=OFF or without the FMOD SDK).");

    if(fmodCore)
    {
        for(const auto* fileName : {"fmod.dll", "fmodstudio.dll"})
        {
            const fs::path source = request.runtimeDirectory / fileName;
            Require(!IsReparsePoint(source), std::string("The FMOD runtime ") + fileName + " must be an ordinary file.");
            copyRuntimeFile(source, fileName);
            deploymentFiles.emplace_back(fileName);
        }

        // Not a legal statement: the obligations are the game author's, under the FMOD license they hold.
        Log::Info("Build Game: deployed the FMOD runtime (fmod.dll, fmodstudio.dll). Firelight's FMOD license governs "
            "redistributing them; check its attribution and title-registration requirements before release.");
    }

    // Steam Audio's runtime, only for saved settings that enable it, and never without FMOD or its notices. Embree is linked into phonon.dll.
    if(request.projectSettings.audio.steamAudio)
    {
        Require(fmodCore, "Project Settings enable Steam Audio, but the runtime directory has no FMOD runtime.");

        for(const auto* fileName : {"phonon.dll", "phonon_fmod.dll", "SteamAudio-THIRDPARTY.md"})
        {
            const fs::path source = request.runtimeDirectory / fileName;
            Require(fs::is_regular_file(source) && !IsReparsePoint(source), std::string("Project Settings enable Steam Audio, but the runtime "
                "directory has no ") + fileName + " (EngineRuntime was built without the Steam Audio SDK). Turn Steam Audio off in Project Settings, "
                "or build EngineRuntime with the SDK.");
            copyRuntimeFile(source, fileName);
            deploymentFiles.emplace_back(fileName);
        }

        Log::Info("Build Game: deployed Steam Audio (phonon.dll, phonon_fmod.dll, SteamAudio-THIRDPARTY.md). Keep its Apache-2.0 notices with the game.");
    }

    // EngineRuntime's build writes RuntimeNotices.txt, plus the app-local MSVC redistributables for Release.
    // Keep this list explicit: arbitrary DLLs/files in a runtime directory are never deployed.
    for(const auto* fileName : {
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll", "vcomp140.dll",
        "RuntimeNotices.txt"})
    {
        if(fs::exists(request.runtimeDirectory / fileName))
        {
            copyRuntimeFile(request.runtimeDirectory / fileName, fileName);
            deploymentFiles.emplace_back(fileName);
        }
    }

    PackagedBootstrap::WriteStartup(stagingDirectory / "Game.startup", sceneAssetID, request.settings.title);
    const auto startupConfiguration = PackagedBootstrap::ReadStartupConfiguration(
        stagingDirectory / "Game.startup");
    Require(startupConfiguration.scene == sceneAssetID && startupConfiguration.title == request.settings.title,
        "Packaged startup configuration differs from build settings.");
    std::string ownershipManifest;
    for (const auto& file : deploymentFiles)
    {
        ownershipManifest += file + "\n";
    }
    WriteText(stagingDirectory / "BuildGame.files", ownershipManifest);
}

}

bool ValidateTitle(std::string_view title, std::string& error)
{
    error.clear();

    try
    {
        WindowsText::FromUTF8(title);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }

    if (title.empty() || title.size() > 100 || title.front() == ' ' ||
        title.back() == ' ' || title.back() == '.')
    {
        error = "Title must contain 1..100 characters and cannot start/end with spaces or end with a dot.";
    }

    for (unsigned char character : title)
    {
        if (character < 32 || character == 127 ||
            std::string_view("<>:\"/\\|?*").find(char(character)) != std::string_view::npos)
        {
            error = "Title must be a safe UTF-8 Windows filename (no path separators or reserved characters).";
        }
    }

    std::string baseName(title.substr(0, title.find('.')));
    for (auto& character : baseName)
    {
        character = char(std::toupper(static_cast<unsigned char>(character)));
    }

    while (!baseName.empty() && (baseName.back() == ' ' || baseName.back() == '.'))
    {
        baseName.pop_back();
    }

    if (baseName == "CON" || baseName == "PRN" || baseName == "AUX" || baseName == "NUL" ||
        baseName == "CLOCK$" || baseName == "CONIN$" || baseName == "CONOUT$" ||
        (baseName.size() == 4 && (baseName.starts_with("COM") || baseName.starts_with("LPT")) &&
         baseName[3] >= '1' && baseName[3] <= '9'))
    {
        error = "Title is a reserved Windows device name.";
    }

    if (title.size() >= 4)
    {
        std::string extension(title.substr(title.size() - 4));
        for (auto& character : extension)
        {
            character = char(std::tolower(static_cast<unsigned char>(character)));
        }

        if (extension == ".exe")
        {
            error = "Enter the title without the .exe extension.";
        }
    }

    return error.empty();
}

AssetID SceneIdentity(std::string_view persistentID)
{
    Require(!persistentID.empty(), "Current scene has no persistent identity.");

    if (auto assetID = AssetID::Parse(persistentID))
    {
        return *assetID;
    }

    const std::string namespacedIdentity = "Engine.Scene:" + std::string(persistentID);
    auto digest = CookedSurface::Hash(
        std::as_bytes(std::span(namespacedIdentity.data(), namespacedIdentity.size())));
    digest[6] = std::byte((std::to_integer<unsigned>(digest[6]) & 15) | 0x50);
    digest[8] = std::byte((std::to_integer<unsigned>(digest[8]) & 63) | 0x80);

    constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    for (size_t index = 0; index < 16; ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
        {
            uuid += '-';
        }

        auto byte = std::to_integer<unsigned>(digest[index]);
        uuid += hex[byte >> 4];
        uuid += hex[byte & 15];
    }

    return *AssetID::Parse(uuid);
}

struct Service::State
{
    mutable std::mutex mutex;
    Status status;
    bool cancel = false;
};

Service::Service() : m_state(std::make_shared<State>())
{
}

Service::~Service()
{
    Cancel();
} // jthread joins; no scene/renderer/UI borrows exist on the worker.

void Service::Cancel()
{
    std::lock_guard lock(m_state->mutex);
    m_state->cancel = true;
}

Status Service::Poll() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->status;
}

bool Service::Start(Request request, std::string& error)
{
    if (!ValidateTitle(request.settings.title, error))
    {
        return false;
    }
    if (request.settings.output.empty())
    {
        error = "Select an output directory.";
        return false;
    }
    // CAM.4 readiness: a new game renders from one enabled, valid Main Camera. Checked before
    // any staging, so a camera-free scene leaves existing outputs untouched.
    if (std::string cameraError; !ValidateGameplayMainCamera(request.document, &cameraError))
    {
        error = "Build Game requires a Main Camera: " + cameraError;
        return false;
    }
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->status.busy)
        {
            error = "A game build is already running.";
            return false;
        }
    }
    if (m_worker.joinable())
    {
        m_worker.join();
    }
    {
        std::lock_guard lock(m_state->mutex);
        m_state->cancel = false;
        m_state->status = {true, false, 0, "Starting game build..."};
    }

    try
    {
        // The non-mutable lambda owns the complete request snapshot, not live editor state.
        m_worker = std::jthread([state = m_state, request = std::move(request)]()
        {
            fs::path stagingDirectory;
            bool ownsStagingDirectory = false;
            const BuildProgress progress{state->mutex, state->cancel, state->status};

            try
            {
                progress.Report(.02f, "Validating output and current scene snapshot...");
                std::string validationError;
                Require(ValidateSceneDocumentPersistence(request.document, &validationError), validationError);
                const auto sceneAssetID = SceneIdentity(request.document.id);
                const auto outputDirectory = PrepareOutputDirectory(request);

                const auto stagingID = AssetID::Generate();
                Require(bool(stagingID), "Cannot generate staging identity.");
                stagingDirectory = outputDirectory.parent_path() /
                    (outputDirectory.filename().string() + ".building-" + stagingID->ToString());
                Require(fs::create_directory(stagingDirectory), "Cannot create private staging directory.");
                ownsStagingDirectory = true;

                BuildAssets build;
                CookMeshAssets(request, build, progress);
                CookSceneAssets(request, sceneAssetID, build, progress);
                // After the scene: scene cooking treats every earlier asset as a mesh input.
                CookScriptAssets(request, build, progress);
                CookAudioBanks(request, build, progress);
                CookAcousticMaterials(request, build);
                CookProbeBatches(request, build);
                CompilePackagedShaders(request, build, progress);
                WriteAndVerifyArchives(request, sceneAssetID, stagingDirectory, build, progress);
                DeployRuntimeFiles(request, sceneAssetID, stagingDirectory, progress);

                progress.Report(.98f, "Publishing validated deployment...");
                // Keep cancellation, destination revalidation, both renames and success reporting
                // under the same lock. No new cancellation window opens after publication.
                std::lock_guard lock(state->mutex);
                Require(!state->cancel, "Build canceled before publication; previous output preserved.");
                CheckDestination(outputDirectory);
                fs::path previousOutput;
                if (fs::exists(outputDirectory))
                {
                    previousOutput = outputDirectory.parent_path() /
                        (outputDirectory.filename().string() + ".previous-" + stagingID->ToString());
                    fs::rename(outputDirectory, previousOutput);
                }
                try
                {
                    fs::rename(stagingDirectory, outputDirectory);
                    stagingDirectory.clear();
                }
                catch (...)
                {
                    if (!previousOutput.empty())
                    {
                        fs::rename(previousOutput, outputDirectory);
                    }
                    throw;
                }
                state->status = {false, true, 1,
                    "Build succeeded: " + UTF8Path(outputDirectory / fs::u8path(request.settings.title + ".exe")) +
                    (previousOutput.empty() ? "" : "; previous output retained at " + previousOutput.string())};
            }
            catch (const std::exception& exception)
            {
                // Only this worker's successfully claimed staging directory may be removed.
                if (ownsStagingDirectory && !stagingDirectory.empty())
                {
                    std::error_code ignored;
                    fs::remove_all(stagingDirectory, ignored);
                }
                std::lock_guard lock(state->mutex);
                state->status = {false, false, 0, exception.what()};
            }
        });
    }
    catch (const std::exception& exception)
    {
        std::lock_guard lock(m_state->mutex);
        m_state->status = {false, false, 0, exception.what()};
        error = exception.what();
        return false;
    }
    return true;
}
}
