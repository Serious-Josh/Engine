#pragma once

#include <d3d12.h>

class Mesh;
struct RayTracingGeometryCounts;

namespace RendererDisplacement
{
    struct GeometryBinding
    {
        const Mesh* mesh = nullptr;
        UINT vertexCount = 0;
        UINT indexCount = 0;

        D3D12_VERTEX_BUFFER_VIEW vertexView{};

        D3D12_INDEX_BUFFER_VIEW indexView{};
    };

    // Validates geometry and constructs borrowed views only. The caller must
    // establish mesh lifetime and recording/submission eligibility separately.
    // Does not publish Mesh counts or mutate resources. Output is unchanged on failure.
    bool TryBuildGeometryBinding(
        const Mesh& mesh,
        const RayTracingGeometryCounts& targetCounts,
        GeometryBinding& binding);
}
