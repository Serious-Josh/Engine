#pragma once

class Mesh;
struct DisplacementMeshSettings;
struct DisplacementGPUIndexLayout;
struct RayTracingGeometryCounts;

namespace RendererDisplacement
{
    // Borrowed inputs only. Layout construction can modify layout on failure;
    // counts are assigned only after all original validation checks succeed.
    bool BuildDisplacementRequestLayout(
        const Mesh& mesh,
        const DisplacementMeshSettings& settings,
        DisplacementGPUIndexLayout& layout,
        RayTracingGeometryCounts& counts);
}
