#include "Engine/BuildGame.h"
#include "Engine/PackagedBootstrap.h"
#include "assets/CookedScene.h"
#include "Scene/RenderView.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetResolver.h"
#include "Renderer/Shader/ShaderLibrary.h"

#include <Windows.h>
#include "Platform/Windows/WindowsText.h"

#include <algorithm>
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
    while (std::getline(manifest, fileName))
    {
        Require(fs::u8path(fileName).filename() == fs::u8path(fileName) && !fileName.empty(),
            "Invalid deployment ownership list.");
        ownedFiles.insert(fileName);
    }

    for (const auto& entry : fs::directory_iterator(outputDirectory))
    {
        // Keep filename conversion behind the existing ordinary-file checks.
        Require(entry.is_regular_file() && !entry.is_symlink() &&
            ownedFiles.contains(UTF8Path(entry.path().filename())),
            "Output contains unrelated files; choose a new directory. Nothing was overwritten.");
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
    if (!request.document.environmentReference.empty())
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

void CompilePackagedShaders(const Request& request, BuildAssets& build, const BuildProgress& progress)
{
    progress.Report(.55f, "Compiling packaged shaders offline...");
    auto shaderPayload = ShaderLibrary::CookBootstrap(request.shaderRoot);
    Require(shaderPayload.size() <= 64ull * 1024 * 1024,
        "Shader bootstrap exceeds the runtime 64 MiB limit.");
    build.Append({PackagedBootstrap::ShaderID(), EngineAssetPak::Type::Shader, 1, std::move(shaderPayload)});
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

    deploymentFiles.emplace_back("AngelScriptNotices.txt");

    // App-local Release redistributables and notices accompany portable runtime inputs.
    // Keep this list explicit: arbitrary DLLs/files in a runtime directory are never deployed.
    for(const auto* fileName : {
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll", "vcomp140.dll",
        "RuntimeNotices.txt", "sdk_license.rtf", "sdk_third_party_notices.rtf"})
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
