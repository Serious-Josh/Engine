#include "Engine/VoxelArchiveStore.h"

#include "assets/ArchiveCookedMeshProvider.h"
#include "assets/AssetID.h"

#include <exception>

namespace VoxelScene
{
ArchiveAssetStore::ArchiveAssetStore(std::shared_ptr<const CookedMeshSource::ArchiveProvider> archive, std::string currentManifest) :
    m_archive(std::move(archive))
{
    if(!currentManifest.empty())
    {
        m_current = std::move(currentManifest);
    }
}

std::optional<std::string> ArchiveAssetStore::GetCurrentManifest() const
{
    return m_current;
}

std::expected<std::vector<std::byte>, std::string> ArchiveAssetStore::Read(std::string_view reference) const
{
    const std::optional<AssetID> id = AssetID::ParseReference(reference);

    if(!id || id->ToReference() != reference)
    {
        return std::unexpected("The voxel asset reference " + std::string(reference) + " is not an archive asset reference.");
    }

    if(!m_archive)
    {
        return std::unexpected("No game archive holds the voxel asset " + std::string(reference) + ".");
    }

    // ReadEntry refuses an absent entry, another type or payload version, or anything larger than a voxel asset can be, before it
    // allocates; the archive reader then checks the entry's SHA-256. Every failure is the asset being unreadable, never other bytes.
    try
    {
        return m_archive->ReadEntry(*id, EngineAssetPak::Type::Resource, ArchivedAssetPayloadVersion, MaximumAssetBytes);
    }
    catch(const std::exception& exception)
    {
        return std::unexpected("The archived voxel asset " + std::string(reference) + " cannot be read: " + exception.what());
    }
}

std::expected<std::string, std::string> ArchiveAssetStore::Stage(std::span<const std::byte>)
{
    return std::unexpected(std::string("A game archive's voxel assets are read-only."));
}

std::expected<void, std::string> ArchiveAssetStore::Publish(std::span<const std::string>, std::string_view)
{
    return std::unexpected(std::string("A game archive's voxel assets are read-only."));
}

void ArchiveAssetStore::Discard(std::span<const std::string>) noexcept
{
}
}
