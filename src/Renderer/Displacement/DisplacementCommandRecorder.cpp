#include "Renderer/Displacement/DisplacementCommandRecorder.h"

#include "Renderer/DisplacementGPUPatchBuffer.h"
#include "Renderer/Geometry/DisplacementMeshGenerator.h"
#include "Renderer/Passes/DisplacementIndicesPass.h"
#include "Renderer/Passes/DisplacementVerticesPass.h"

bool RendererDisplacement::RecordPatchAndCompute(
    ID3D12GraphicsCommandList* commandList,
    DisplacementGPUPatchBuffer& patchBuffer,
    UploadBuffer& uploadBuffer,
    DisplacementVerticesPass& verticesPass,
    DisplacementIndicesPass& indicesPass,
    Mesh& mesh,
    Texture& heightmap,
    const DisplacementGPUIndexLayout& layout,
    const DisplacementMeshSettings& settings)
{
    if (!patchBuffer.RecordUpdate(commandList, uploadBuffer, layout) ||
        !verticesPass.Execute(commandList, mesh, heightmap, uploadBuffer, settings.patchesPerAxis,
                              settings.subdivisionsPerPatch, settings.displacementScale,
                              settings.displacementMidpoint) ||
        !indicesPass.Execute(commandList, uploadBuffer, mesh, settings.patchesPerAxis, settings.subdivisionsPerPatch,
                             patchBuffer.GetDescriptorIndex()))
    {
        return false;
    }

    return true;
}
