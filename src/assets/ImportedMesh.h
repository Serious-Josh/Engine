#pragma once

#include "Renderer/Vertex.h"
#include "Math/Vector3.h"
#include "assets/MeshMetadata.h"

#include <cstdint>
#include <vector>

struct ImportedMesh
{
    std::vector<Vertex> vertices;

    std::vector<uint32_t> indices;

    MeshMetadata metadata;

    Vector3 boundsMin{};
    Vector3 boundsMax{};
};
