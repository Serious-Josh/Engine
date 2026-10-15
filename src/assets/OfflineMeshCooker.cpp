#include "assets/AssetRegistrySerializer.h"
#include "assets/CookedMesh.h"
#include "assets/DevelopmentMeshCookRecipe.h"
#include "assets/MeshCookValidation.h"
#include "assets/ModelImporter.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ProjectMeshPreparationService.h"
#include "Project/ProjectLocator.h"
#include "Renderer/Preview/MaterialThumbnailValidation.h"

#include <Windows.h>
#include <assimp/version.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::ordered_json;

    void Require(bool condition, const std::string& message)
    {
        if(!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WindowsCheck(bool condition, const char* operation)
    {
        if(!condition)
        {
            throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ").");
        }
    }

    struct Handle
    {
        HANDLE value = INVALID_HANDLE_VALUE;

        explicit Handle(HANDLE handle) : value(handle)
        {
            WindowsCheck(value != INVALID_HANDLE_VALUE, "Cannot open cooker file/directory");
        }

        ~Handle() { CloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    using Pin = std::unique_ptr<Handle>;

    Pin ReadPin(const fs::path& path)
    {
        auto pin = std::make_unique<Handle>(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        WindowsCheck(GetFileInformationByHandle(pin->value, &info) != FALSE, "Cannot inspect cooker input");
        Require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
            "[source] Expected an ordinary file: " + path.string());
        return pin;
    }

    // Same no-delete ancestor pinning convention as ProjectMeshImportService.
    class DirectoryPins
    {
    public:
        void PinPath(const fs::path& path)
        {
            fs::path prefix;

            for(const auto& part : path)
            {
                prefix /= part;

                if(!prefix.has_root_directory() || std::find(m_paths.begin(), m_paths.end(), prefix) != m_paths.end())
                {
                    continue;
                }

                auto pin = std::make_unique<Handle>(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                BY_HANDLE_FILE_INFORMATION info{};
                WindowsCheck(GetFileInformationByHandle(pin->value, &info) != FALSE, "Cannot inspect cooker directory");
                Require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                    "[path] Cooker directories must be ordinary and non-linked: " + prefix.string());
                m_paths.push_back(prefix);
                m_pins.push_back(std::move(pin));
            }
        }

    private:
        std::vector<fs::path> m_paths;
        std::vector<Pin> m_pins;
    };

    std::string Digest(const fs::path& path)
    {
        // Fresh utility instance: never accept a cached size/timestamp digest.
        MaterialThumbnailValidation validation;
        return validation.Digest(path);
    }

    std::string Hex(const CookedMesh::Evidence& evidence)
    {
        std::string result;
        constexpr char digits[] = "0123456789abcdef";

        for(auto byte : evidence.payloadSHA256)
        {
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }

        return result;
    }

    void WriteNew(const fs::path& path, std::span<const std::byte> bytes)
    {
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        size_t offset = 0;

        while(offset < bytes.size())
        {
            const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t(1024 * 1024)));
            DWORD written = 0;
            WindowsCheck(WriteFile(file.value, bytes.data() + offset, count, &written, nullptr) != FALSE && written == count,
                "Cannot write complete cooker output");
            offset += count;
        }

        WindowsCheck(FlushFileBuffers(file.value) != FALSE, "Cannot flush cooker output");
    }

    std::vector<std::byte> ReadBounded(const fs::path& path, size_t limit)
    {
        const auto length = fs::file_size(path);
        Require(length <= limit, "[verification] Staged output exceeds read limit.");
        std::vector<std::byte> bytes(static_cast<size_t>(length));
        std::ifstream file(path, std::ios::binary);
        Require(bool(file), "[I/O] Cannot open staged output: " + path.string());
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Require(bool(file) && file.gcount() == static_cast<std::streamsize>(bytes.size()), "[I/O] Cannot read complete staged output.");
        return bytes;
    }

    Json Recipe()
    {
        return DevelopmentMeshCookRecipe::Describe(ModelImporter::kImportRevision,
            std::to_string(aiGetVersionMajor()) + "." + std::to_string(aiGetVersionMinor()) + "." +
            std::to_string(aiGetVersionPatch()), aiGetCompileFlags());
    }

    struct Source
    {
        AssetRecord record;
        fs::path path;
        std::string digest;
        std::string identity;
        Pin pin;
    };

    void Revalidate(const Source& source, const ProjectPaths& paths, const ProjectAssetRegistry& registry)
    {
        const auto resolved = ProjectAssetResolver::Resolve(paths, registry, source.record.id);
        Require(resolved.status == AssetAvailability::Available && resolved.resolvedPath == source.path,
            "[freshness] Registered source resolution changed: " + source.record.id.ToString());
        Require(ProjectMeshPreparationService::InspectSourceIdentity(source.path) == source.identity &&
            Digest(source.path) == source.digest, "[freshness] Source changed while cooking: " + source.record.id.ToString());
    }

    Json CookOne(const Source& source, const fs::path& stage, const Json& recipe)
    {
        const CookedMesh::Policy policy;
        ImportedMesh geometry;

        try
        {
            geometry = ModelImporter::LoadFlattened(source.path, MeshUVOrientation::FlipV, policy.maxDecodedBytes);
        }
        catch(const std::exception& error)
        {
            throw std::runtime_error(std::string("[import] ") + error.what());
        }

        auto encoded = CookedMesh::Encode(geometry, policy);
        Require(encoded.status == CookedMesh::Status::Success, "[encode status " + std::to_string(int(encoded.status)) + "] " +
            std::string(encoded.message));
        const auto relative = fs::path("Meshes") / (source.record.id.ToString() + ".bin");
        const auto output = stage / relative;
        WriteNew(output, encoded.payload);

        // Verify bytes read back from disk, not just the encoder's in-memory result.
        const auto pin = ReadPin(output);
        const auto persisted = ReadBounded(output, policy.maxPayloadBytes);
        Require(persisted == encoded.payload, "[verification] Written payload differs from encoded bytes.");
        const auto decoded = CookedMesh::Decode(persisted, policy);
        Require(decoded.status == CookedMesh::Status::Success, "[decode status " + std::to_string(int(decoded.status)) + "] " +
            std::string(decoded.message));
        Require(decoded.evidence == encoded.evidence && MeshCookValidation::ExactlyEqual(geometry, *decoded.geometry),
            "[verification] Cooked geometry or identity differs from importer output.");
        const auto digest = Hex(encoded.evidence);
        Require(Digest(output) == digest, "[verification] Persisted payload digest differs.");

        return {{"assetID", source.record.id.ToString()}, {"assetType", "mesh"},
            {"sourcePath", source.record.relativePath.generic_string()}, {"sourceSHA256", source.digest},
            {"recipeIdentity", recipe.dump()}, {"payloadSHA256", digest}, {"payloadBytes", encoded.payload.size()},
            {"payloadPath", relative.generic_string()}, {"formatVersion", CookedMesh::V1::Version},
            {"cookCompatibility", CookedMesh::V1::CookCompatibility}, {"vertexLayout", CookedMesh::V1::VertexLayout}};
    }

    std::pair<fs::path, std::string> CreateStage(const fs::path& generations)
    {
        const auto prefix = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());

        for(unsigned int attempt = 0; attempt < 100; ++attempt)
        {
            const auto name = prefix + "-" + std::to_string(attempt);
            const auto path = generations / (".staging-" + name);

            if(fs::create_directory(path))
            {
                return {path, "generation-" + name};
            }
        }

        throw std::runtime_error("[staging] Cannot reserve a unique generation directory.");
    }
}

// Debug-only command entry point; no Engine/renderer initialization occurs.
int CookProjectMeshes(const std::filesystem::path& requestedRoot)
{
    fs::path stage;

    try
    {
        const auto root = fs::absolute(requestedRoot.empty() ? ProjectLocator::GetDefaultProjectRoot() : requestedRoot).lexically_normal();
        DirectoryPins directories;
        directories.PinPath(root);
        const ProjectPaths paths(fs::canonical(root));
        const auto registryPin = ReadPin(paths.GetAssetRegistryPath());
        const auto registryDigest = Digest(paths.GetAssetRegistryPath());
        ProjectAssetRegistry registry;
        std::string error;

        if(!AssetRegistrySerializer::LoadFromFile(paths.GetAssetRegistryPath(), registry, &error))
        {
            throw std::runtime_error("[registry] " + error);
        }

        auto meshes = registry.GetAssets(AssetType::Mesh);
        Require(meshes.size() <= 4096, "[admission] Cooker supports at most 4096 registered meshes per generation.");
        std::sort(meshes.begin(), meshes.end(), [](const auto& a, const auto& b) { return a.id.ToString() < b.id.ToString(); });

        const auto cooked = paths.GetRoot() / "Cooked";
        fs::create_directory(cooked);
        directories.PinPath(cooked);
        const auto generations = cooked / "generations";
        fs::create_directory(generations);
        directories.PinPath(generations);
        Handle cookLock(CreateFileW((cooked / ".mesh-cooker.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION lockInfo{};
        WindowsCheck(GetFileInformationByHandle(cookLock.value, &lockInfo) != FALSE, "Cannot inspect cooker lock");
        Require(!(lockInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "[path] Invalid cooker lock.");

        auto reservation = CreateStage(generations);
        stage = reservation.first;
        fs::create_directory(stage / "Meshes");
        auto stagePins = std::make_unique<DirectoryPins>();
        stagePins->PinPath(stage / "Meshes");
        const auto recipe = Recipe();
        Json manifest = {{"schemaVersion", 1}, {"kind", "engine-development-mesh-manifest"},
            {"recipe", recipe}, {"assets", Json::array()}};
        std::vector<Source> sources;
        size_t failures = 0;

        for(const auto& record : meshes)
        {
            try
            {
                const auto resolved = ProjectAssetResolver::Resolve(paths, registry, record.id);
                Require(resolved.status == AssetAvailability::Available, "[source] Registered mesh is unavailable or unsafe.");
                Require(_wcsicmp(resolved.resolvedPath.extension().c_str(), L".fbx") == 0, "[source] Registered mesh must be FBX.");
                directories.PinPath(resolved.resolvedPath.parent_path());
                auto pin = ReadPin(resolved.resolvedPath);
                LARGE_INTEGER size{};
                WindowsCheck(GetFileSizeEx(pin->value, &size) != FALSE, "Cannot inspect mesh source size");
                Require(size.QuadPart > 0 && size.QuadPart <= ProjectMeshPreparationService::Limits{}.sourceBytes,
                    "[admission] Mesh source is empty or exceeds the existing 32 MiB source policy.");

                Source source{record, resolved.resolvedPath, Digest(resolved.resolvedPath),
                    ProjectMeshPreparationService::InspectSourceIdentity(resolved.resolvedPath), std::move(pin)};
                Revalidate(source, paths, registry);
                auto entry = CookOne(source, stage, recipe);
                Revalidate(source, paths, registry);
                manifest["assets"].push_back(std::move(entry));
                sources.push_back(std::move(source));
                std::cout << "Cooked " << record.id.ToString() << " (" << record.relativePath.generic_string() << ").\n";
            }
            catch(const std::exception& failure)
            {
                ++failures;
                std::cerr << "Failed " << record.id.ToString() << " (" << record.relativePath.generic_string() << "): " << failure.what() << '\n';
            }
        }

        Require(failures == 0, "[generation] " + std::to_string(failures) + " asset(s) failed; previous generation remains active.");
        const auto text = manifest.dump(2) + "\n";
        Require(text.size() <= 16 * 1024 * 1024, "[admission] Development manifest exceeds 16 MiB.");
        const auto manifestPath = stage / "mesh-manifest.json";
        WriteNew(manifestPath, std::as_bytes(std::span(text.data(), text.size())));
        const auto persisted = ReadBounded(manifestPath, 16 * 1024 * 1024);
        Require(std::equal(persisted.begin(), persisted.end(), reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())), "[verification] Persisted manifest differs.");
        const auto manifestDigest = Digest(manifestPath);

        for(const auto& source : sources)
        {
            Revalidate(source, paths, registry);
        }

        Require(Digest(paths.GetAssetRegistryPath()) == registryDigest, "[freshness] Registry changed during cooking.");
        const auto published = generations / reservation.second;
        stagePins.reset(); // Release only staging pins so Windows can rename it.
        WindowsCheck(MoveFileExW(stage.c_str(), published.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "Cannot finalize cooked generation");
        stage.clear();

        const Json activation = {{"schemaVersion", 1}, {"generation", (fs::path("generations") / reservation.second).generic_string()},
            {"manifestSHA256", manifestDigest}};
        const auto activationText = activation.dump(2) + "\n";
        const auto temporary = cooked / (".activate-" + reservation.second + ".json");
        WriteNew(temporary, std::as_bytes(std::span(activationText.data(), activationText.size())));
        WindowsCheck(MoveFileExW(temporary.c_str(), (cooked / "active-mesh-generation.json").c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE, "Cannot activate cooked mesh generation");
        std::cout << "Published " << meshes.size() << " registered meshes: " << published.string() << '\n';
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "Mesh cooking failed: " << error.what() << '\n';

        if(!stage.empty())
        {
            std::cerr << "Unpublished staging output retained for inspection: " << stage.string() << '\n';
        }

        return 1;
    }
}
