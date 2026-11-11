#include "Voxel/VoxelAssetStore.h"

#include "Voxel/VoxelBinary.h"

#include <algorithm>
#include <cstdio>

namespace Voxel
{
std::string MemoryAssetStore::ReferenceFor(std::span<const std::byte> bytes)
{
    char text[64];
    std::snprintf(text, sizeof(text), "voxel:%016llx-%zu", static_cast<unsigned long long>(Binary::Fnv1a(bytes)), bytes.size());
    return text;
}

std::optional<std::string> MemoryAssetStore::GetCurrentManifest() const
{
    const std::lock_guard lock(m_mutex);
    return m_current;
}

std::expected<std::vector<std::byte>, std::string> MemoryAssetStore::Read(std::string_view reference) const
{
    const std::lock_guard lock(m_mutex);
    const auto found = m_published.find(reference);

    if(found == m_published.end())
    {
        return std::unexpected("No published asset " + std::string(reference) + ".");
    }

    return found->second;
}

std::expected<std::string, std::string> MemoryAssetStore::Stage(std::span<const std::byte> bytes)
{
    if(bytes.size() > MaximumAssetBytes)
    {
        return std::unexpected("A voxel asset of " + std::to_string(bytes.size()) + " bytes exceeds the " + std::to_string(MaximumAssetBytes) +
            "-byte limit.");
    }

    std::string reference = ReferenceFor(bytes);
    const std::lock_guard lock(m_mutex);

    // Already published with exactly these bytes: nothing to stage.
    if(const auto published = m_published.find(reference);
        published != m_published.end() && std::equal(published->second.begin(), published->second.end(), bytes.begin(), bytes.end()))
    {
        return reference;
    }

    const auto [staged, isNew] = m_staged.try_emplace(reference, bytes.begin(), bytes.end());

    if(!isNew && !std::equal(staged->second.begin(), staged->second.end(), bytes.begin(), bytes.end()))
    {
        return std::unexpected("Reference collision staging " + reference + ".");
    }

    return reference;
}

std::expected<void, std::string> MemoryAssetStore::Publish(std::span<const std::string> staged, std::string_view manifest)
{
    const std::lock_guard lock(m_mutex);

    // Validate everything before changing anything.
    for(const std::string& reference : staged)
    {
        if(!m_staged.contains(reference) && !m_published.contains(reference))
        {
            return std::unexpected("Publishing unknown asset " + reference + ".");
        }
    }

    if(!m_staged.contains(manifest) && !m_published.contains(manifest))
    {
        return std::unexpected("Publishing unknown manifest " + std::string(manifest) + ".");
    }

    for(const std::string& reference : staged)
    {
        if(const auto found = m_staged.find(reference); found != m_staged.end())
        {
            m_published[reference] = std::move(found->second);
            m_staged.erase(found);
        }
    }

    if(const auto found = m_staged.find(manifest); found != m_staged.end())
    {
        m_published[std::string(manifest)] = std::move(found->second);
        m_staged.erase(found);
    }

    m_current = std::string(manifest);
    return {};
}

void MemoryAssetStore::Discard(std::span<const std::string> staged) noexcept
{
    const std::lock_guard lock(m_mutex);

    for(const std::string& reference : staged)
    {
        m_staged.erase(reference);
    }
}

void MemoryAssetStore::ReplacePublished(const std::string& reference, std::vector<std::byte> bytes)
{
    const std::lock_guard lock(m_mutex);
    m_published[reference] = std::move(bytes);
}

void MemoryAssetStore::RemovePublished(const std::string& reference)
{
    const std::lock_guard lock(m_mutex);
    m_published.erase(reference);
}

std::size_t MemoryAssetStore::GetPublishedCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_published.size();
}

std::size_t MemoryAssetStore::GetStagedCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_staged.size();
}
}
