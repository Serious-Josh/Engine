#pragma once

struct ID3D12GraphicsCommandList;
class DisplacementGPUPatchBuffer;
class UploadBuffer;
class DisplacementVerticesPass;
class DisplacementIndicesPass;
class Mesh;
class Texture;
struct DisplacementGPUIndexLayout;
struct DisplacementMeshSettings;

namespace RendererDisplacement
{
    // Caller supplies an active DIRECT recording with the required descriptor
    // heap bound and resources/staging protected for their GPU-use lifetime.
    // True means all three recording operations succeeded, not GPU completion.
    // False may leave partial commands, consumed staging, and changed tracking;
    // Renderer must resolve the whole frame. No local rollback is performed.
    bool RecordPatchAndCompute(
        ID3D12GraphicsCommandList* commandList,
        DisplacementGPUPatchBuffer& patchBuffer,
        UploadBuffer& uploadBuffer,
        DisplacementVerticesPass& verticesPass,
        DisplacementIndicesPass& indicesPass,
        Mesh& mesh,
        Texture& heightmap,
        const DisplacementGPUIndexLayout& layout,
        const DisplacementMeshSettings& settings);
}
