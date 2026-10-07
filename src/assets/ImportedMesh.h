#pragma once

#include "Renderer/Vertex.h"

#include <cstdint>
#include <vector>

struct ImportedMesh
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};