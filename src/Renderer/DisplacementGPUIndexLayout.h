#pragma once

#include "Renderer/DisplacementGPUPatch.h"
#include "Renderer/Geometry/DisplacementMeshGenerator.h"

#include <cstdint>
#include <vector>

struct DisplacementGPUIndexLayout
{
    std::vector<DisplacementGPUPatch> patches;

    std::uint32_t totalIndexCount = 0;
};

class DisplacementGPUIndexLayoutBuilder
{
    public:
    // Calculates GPU patch metadata without generating
    // vertices or triangle indices.
    //
    // Returns false for invalid settings or if the
    // resulting index layout exceeds uint32_t limits.
    static bool Build(
        const DisplacementMeshSettings& settings,
        DisplacementGPUIndexLayout& output);
};
