#include "Renderer/Displacement/DisplacementRequestValidation.h"

#include "Core/Log.h"
#include "Renderer/DisplacementGPUIndexLayout.h"
#include "Renderer/Mesh.h"
#include "Renderer/RayTracing/RayTracingAccelerationStructure.h"

#include <climits>
#include <cstddef>
#include <cstdint>

bool RendererDisplacement::BuildDisplacementRequestLayout(
    const Mesh& mesh,
    const DisplacementMeshSettings& settings,
    DisplacementGPUIndexLayout& layout,
    RayTracingGeometryCounts& counts)
{
    if (!DisplacementGPUIndexLayoutBuilder::Build(settings, layout))
    {
        Log::Error("Failed to build GPU displacement index layout.");
        return false;
    }

    const std::uint64_t gridResolution =

        static_cast<std::uint64_t>(settings.patchesPerAxis) * settings.subdivisionsPerPatch;
    // Bound the side before squaring; also enforce the 32-bit vertex-view limit.
    const std::uint64_t side = gridResolution + 1ull;

    if (gridResolution == 0 || gridResolution >= UINT_MAX || side > (UINT_MAX / 48u) / side)
    {
        Log::Error("Invalid GPU displacement vertex count.");
        return false;
    }

    const std::uint64_t vertexCount = (gridResolution + 1ull) * (gridResolution + 1ull);

    const std::uint64_t indexCount = layout.totalIndexCount;

    if (vertexCount == 0 || vertexCount > mesh.GetVertexCapacity() || indexCount == 0 ||
        indexCount > mesh.GetIndexCapacity())
    {
        Log::Error("GPU displacement geometry exceeds mesh capacity.");
        return false;
    }

    if (indexCount % 3 != 0 || indexCount > UINT_MAX / sizeof(unsigned int) || mesh.GetVertexStride() != 48 ||

        mesh.GetIndexFormat() != DXGI_FORMAT_R32_UINT)
    {
        Log::Error("GPU displacement geometry exceeds mesh capacity.");
        return false;
    }

    if (!mesh.GetVertexBufferResource() || !mesh.GetIndexBufferResource() || mesh.GetVertexBufferAddress() == 0 ||

        mesh.GetIndexBufferAddress() == 0)
    {
        Log::Error("GPU displacement geometry exceeds mesh capacity.");
        return false;
    }

    if (mesh.GetVertexBufferResource()->GetDesc().Width < vertexCount * 48u ||

        mesh.GetIndexBufferResource()->GetDesc().Width < indexCount * sizeof(unsigned int))
    {
        Log::Error("GPU displacement geometry exceeds mesh capacity.");
        return false;
    }

    counts = {static_cast<size_t>(vertexCount), static_cast<size_t>(indexCount)};

    return true;
}
