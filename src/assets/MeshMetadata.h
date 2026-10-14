#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct MeshSubmesh
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t materialSlot = 0;

    std::string name;
    bool operator==(const MeshSubmesh&) const = default;
};

struct MeshMaterialSlot
{
    // Deterministic within this asset/import: default, or material:<source index>.
    // Source material reordering on reimport requires future explicit remapping.
    std::string id;
    std::string name;

    std::uint32_t sourceMaterialIndex = UINT32_MAX; // No source material for procedural geometry.
    bool operator==(const MeshMaterialSlot&) const = default;
};

struct MeshMetadata
{
    std::vector<MeshSubmesh> submeshes;
    std::vector<MeshMaterialSlot> materialSlots;
    bool operator==(const MeshMetadata&) const = default;
};

inline MeshMetadata DefaultMeshMetadata(std::uint32_t indexCount)
{
    return {{{0, indexCount, 0, "Mesh"}}, {{"default", "Default Material", UINT32_MAX}}};
}

inline bool ValidateMeshMetadata(const MeshMetadata& metadata, size_t indexCount, std::string& error)
{
    error.clear();
    std::unordered_set<std::string> identifiers;

    for(const auto& slot : metadata.materialSlots)
    {
        if(slot.id.empty() || slot.name.empty() || !identifiers.insert(slot.id).second)
        {
            error = "Mesh material slots require unique identifiers and readable names.";
            return false;
        }
    }

    size_t nextIndex = 0;

    for(const auto& submesh : metadata.submeshes)
    {
        if(submesh.name.empty() || submesh.materialSlot >= metadata.materialSlots.size() ||
            submesh.firstIndex != nextIndex || submesh.firstIndex % 3 != 0 ||
            submesh.indexCount == 0 || submesh.indexCount % 3 != 0 ||
            nextIndex > indexCount || submesh.indexCount > indexCount - nextIndex)
        {
            error = "Mesh submesh ranges must cover ordered triangle ranges with valid material slots.";
            return false;
        }

        nextIndex += submesh.indexCount;
    }

    if(metadata.submeshes.empty() || metadata.materialSlots.empty() || nextIndex != indexCount)
    {
        error = "Mesh metadata must cover the complete index buffer.";
        return false;
    }

    return true;
}
