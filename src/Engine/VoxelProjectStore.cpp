#include "Engine/VoxelProjectStore.h"

#include "Project/ProjectPaths.h"
#include "assets/AssetID.h"
#include "assets/AssetRecord.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"
#include "Voxel/VoxelManifest.h"
#include "Voxel/VoxelRegion.h"

#include <cstring>
#include <fstream>

namespace VoxelScene
{
namespace
{
    const std::filesystem::path VoxelDirectory = "Content/Voxels";

    bool HasMagic(std::span<const std::byte> bytes, const char* magic) noexcept
    {
        return bytes.size() >= 8 && std::memcmp(bytes.data(), magic, 8) == 0;
    }
}

ProjectAssetStore::ProjectAssetStore(const ProjectPaths& paths, ProjectAssetRegistry& registry, std::string currentManifest) :
    ProjectAssetStore(paths, static_cast<const ProjectAssetRegistry&>(registry), std::move(currentManifest))
{
    m_registry = &registry;
}

ProjectAssetStore::ProjectAssetStore(const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::string currentManifest) :
    m_paths(paths)
{
    for(const AssetType type : {AssetType::VoxelRegion, AssetType::VoxelManifest})
    {
        for(const AssetRecord& record : registry.GetAssets(type))
        {
            m_published[record.id.ToReference()] = paths.GetRoot() / record.relativePath;
        }
    }

    if(!currentManifest.empty())
    {
        m_current = std::move(currentManifest);
    }
}

ProjectAssetStore::~ProjectAssetStore()
{
    // A store destroyed with assets still staged never publishes them.
    std::vector<std::string> staged;
    {
        const std::lock_guard lock(m_mutex);
        for(const auto& [reference, asset] : m_staged)
        {
            staged.push_back(reference);
        }
    }
    Discard(staged);
}

std::optional<std::string> ProjectAssetStore::GetCurrentManifest() const
{
    const std::lock_guard lock(m_mutex);
    return m_current;
}

std::expected<std::vector<std::byte>, std::string> ProjectAssetStore::Read(std::string_view reference) const
{
    std::filesystem::path path;
    {
        const std::lock_guard lock(m_mutex);
        const auto found = m_published.find(reference);
        if(found == m_published.end())
        {
            return std::unexpected("No registered voxel asset " + std::string(reference) + ".");
        }
        path = found->second;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if(error)
    {
        return std::unexpected("The voxel asset file " + path.generic_string() + " cannot be read.");
    }

    // Refuse before allocating: nothing larger can be a valid voxel asset.
    if(size > MaximumAssetBytes)
    {
        return std::unexpected("The voxel asset file " + path.generic_string() + " is larger than any voxel asset can be.");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return std::unexpected("The voxel asset file " + path.generic_string() + " cannot be read.");
    }

    return bytes;
}

std::expected<std::string, std::string> ProjectAssetStore::Stage(std::span<const std::byte> bytes)
{
    if(m_registry == nullptr)
    {
        return std::unexpected(std::string("This voxel asset store is read-only."));
    }

    if(bytes.size() > MaximumAssetBytes)
    {
        return std::unexpected("A voxel asset of " + std::to_string(bytes.size()) + " bytes exceeds the " + std::to_string(MaximumAssetBytes) +
            "-byte limit.");
    }

    const bool manifest = HasMagic(bytes, "ENGVOXMF");
    if(!manifest && !HasMagic(bytes, "ENGVOXRG"))
    {
        return std::unexpected(std::string("Only voxel regions and manifests can be staged."));
    }

    const std::optional<AssetID> id = AssetID::Generate();
    if(!id)
    {
        return std::unexpected(std::string("Cannot generate an asset ID."));
    }

    const std::string_view extension = manifest ? Voxel::ManifestFormat::Extension : Voxel::RegionFormat::Extension;
    const std::filesystem::path relativePath = VoxelDirectory / (id->ToString() + std::string(extension));
    const std::filesystem::path destination = m_paths.GetRoot() / relativePath;
    std::filesystem::path temporary = destination;
    temporary += ".staging";
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if(!file.flush())
        {
            file.close();
            std::filesystem::remove(temporary, error);
            return std::unexpected("The voxel asset file " + destination.generic_string() + " cannot be written.");
        }
    }

    // The file is complete before it has its final name, so a crash never leaves a truncated asset under that name.
    std::filesystem::rename(temporary, destination, error);
    if(error)
    {
        std::filesystem::remove(temporary, error);
        return std::unexpected("The voxel asset file " + destination.generic_string() + " cannot be placed.");
    }

    std::string reference = id->ToReference();
    const std::lock_guard lock(m_mutex);
    m_staged[reference] = {relativePath, manifest};
    return reference;
}

std::expected<void, std::string> ProjectAssetStore::Publish(std::span<const std::string> staged, std::string_view manifest)
{
    const std::lock_guard lock(m_mutex);

    if(m_registry == nullptr)
    {
        return std::unexpected(std::string("This voxel asset store is read-only."));
    }

    ProjectAssetRegistry next = *m_registry;

    // Register everything in a copy first: the live registry changes only after the one atomic save succeeds.
    std::vector<std::string> publishing(staged.begin(), staged.end());
    publishing.emplace_back(manifest);

    for(const std::string& reference : publishing)
    {
        if(m_published.contains(reference))
        {
            continue;
        }

        const auto found = m_staged.find(reference);
        const std::optional<AssetID> id = AssetID::ParseReference(reference);
        if(found == m_staged.end() || !id)
        {
            return std::unexpected("Publishing unknown voxel asset " + reference + ".");
        }

        const AssetType type = found->second.manifest ? AssetType::VoxelManifest : AssetType::VoxelRegion;
        if(!next.Register(AssetRecord{*id, type, found->second.relativePath}))
        {
            return std::unexpected("Cannot register voxel asset " + reference + ".");
        }
    }

    std::string registryError;
    if(!AssetRegistrySerializer::SaveToFile(next, m_paths.GetAssetRegistryPath(), &registryError))
    {
        return std::unexpected("Cannot save the asset registry: " + registryError);
    }

    *m_registry = std::move(next);

    for(const std::string& reference : publishing)
    {
        if(const auto found = m_staged.find(reference); found != m_staged.end())
        {
            m_published[reference] = m_paths.GetRoot() / found->second.relativePath;
            m_staged.erase(found);
        }
    }

    m_current = std::string(manifest);
    return {};
}

void ProjectAssetStore::Discard(std::span<const std::string> staged) noexcept
{
    const std::lock_guard lock(m_mutex);

    for(const std::string& reference : staged)
    {
        const auto found = m_staged.find(reference);
        if(found == m_staged.end())
        {
            continue;
        }

        std::error_code error;
        std::filesystem::remove(m_paths.GetRoot() / found->second.relativePath, error);
        m_staged.erase(found);
    }
}

std::size_t ProjectAssetStore::GetStagedCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_staged.size();
}
}
