#pragma once

#include "assets/ImportedMesh.h"
#include "assets/MeshPreparationPolicy.h"
#include "assets/MeshImportUnits.h"
#include <optional>

#include <filesystem>
#include <vector>
#include <limits>

class ModelImporter
{
    public:
    static constexpr std::uint32_t kImportRevision = 3; // FlipV-capable import with submesh/material-slot metadata.

    // Default preserves Assimp UVs and existing caller behavior. FlipV applies
    // v = 1 - v before tangent generation, including every node mesh occurrence.
    static std::vector<ImportedMesh> Load(const std::filesystem::path& path,
        MeshUVOrientation uvOrientation = MeshUVOrientation::PreserveSource,
        MeshImportUnits units = {});

    // FBX UnitScaleFactor is centimetres per source unit; Assimp exposes the FBX default
    // (one centimetre) when the property is omitted. Other formats have no FileUnits contract.
    static double InspectFBXUnitScale(const std::filesystem::path& path);

    // Same static import contract as Load(), with all node occurrences concatenated.
    // Throws std::runtime_error with file/node/mesh context on invalid geometry.
    // Optional service budget bounds Engine-owned conversion/flattening output.
    // Assimp's parser allocations are external and remain an admission estimate.
    static ImportedMesh LoadFlattened(const std::filesystem::path& path,
        MeshUVOrientation uvOrientation = MeshUVOrientation::PreserveSource,
        size_t geometryByteLimit = (std::numeric_limits<size_t>::max)(), MeshImportUnits units = {});
};
