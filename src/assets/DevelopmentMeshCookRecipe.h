#pragma once

#include "assets/CookedMesh.h"
#include <nlohmann/json.hpp>

// Development manifest recipe schema shared by its writer and reader. Explicit
// arguments keep importer/Assimp dependencies out of the provider.
namespace DevelopmentMeshCookRecipe
{
    inline nlohmann::ordered_json Describe(std::uint32_t importerRevision, const std::string& assimpVersion,
        std::uint32_t assimpCompileFlags)
    {
        return {{"recipeVersion", 1}, {"meshFormatVersion", CookedMesh::V1::Version},
            {"cookCompatibility", CookedMesh::V1::CookCompatibility}, {"importerRevision", importerRevision},
            {"uvOrientation", "FlipV"}, {"vertexLayout", CookedMesh::V1::VertexLayout}, {"indexBytes", CookedMesh::V1::IndexBytes},
            {"assimpVersion", assimpVersion}, {"assimpCompileFlags", assimpCompileFlags},
            {"assimpProcessing", "Triangulate|ValidateDataStructure|GenSmoothNormals"},
            {"conversion", "depth-first-occurrences|baked-affine-transforms|inverse-transpose-normals|mirrored-winding|"
                "uv0-flipv-before-tangents|generated-seam-split-tangents|global-u32-indices|source-material-slots"}};
    }
}
