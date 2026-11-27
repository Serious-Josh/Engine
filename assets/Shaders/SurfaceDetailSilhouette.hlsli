#ifndef ENGINE_SURFACE_DETAIL_SILHOUETTE_HLSLI
#define ENGINE_SURFACE_DETAIL_SILHOUETTE_HLSLI

// Silhouette parallax occlusion mapping (SPOM): where the view ray leaves the mesh before it meets the relief, the pixel
// is discarded, so the relief cuts the mesh's outline (a slot reaching a box's edge shows as a notch). Without it such a
// pixel still draws the relief found past the edge, and the outline stays the mesh's straight edge. Design:
// Documentation/Rendering/POM_Improvement_Investigation.md (decision 3).
//
// Only real edges cut: those where the surface ends or turns sharply, marked per triangle when the mesh is created
// (Mesh::GetSilhouetteEdgeDescriptorIndex: bits 0, 1 and 2 for the edges from vertex 0 to 1, 1 to 2 and 2 to 0). A UV
// seam, a texture tiled several times over a face, the diagonal inside a quad or a smooth curved surface never cut.
//
// The test is against the pixel's own triangle: the relief point lies past one of its marked edges in UV space. A point
// that crosses an unmarked edge into the neighbouring triangle (a quad's diagonal) and leaves the mesh through that
// triangle's edge is not caught; near a quad's corners a few such pixels keep the straight outline.

#include "RayTracingInstanceData.hlsli"

// Whether the relief point at displacedUV, found from triangle primitiveIndex of the instance, lies outside the mesh:
// past one of the triangle's silhouette edges in UV space. False for a mesh without silhouette edges.
bool SurfaceDetailLeavesSilhouette(RayTracingInstanceData instance, uint primitiveIndex, float2 displacedUV)
{
    if(instance.silhouetteEdgeBufferIndex == 0xFFFFFFFFu || instance.indexed == 0u)
    {
        return false;
    }

    ByteAddressBuffer silhouetteEdges =
        ResourceDescriptorHeap[NonUniformResourceIndex(instance.silhouetteEdgeBufferIndex)];
    uint edges = silhouetteEdges.Load(primitiveIndex * 4u) & 7u;

    if(edges == 0u)
    {
        return false;
    }

    ByteAddressBuffer indexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.indexBufferIndex)];
    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vertexBufferIndex)];
    uint3 indices = indexBuffer.Load3(primitiveIndex * 12u);
    uint3 uvAddresses = indices * instance.vertexStride + instance.uvOffset;
    float2 uv0 = asfloat(vertexBuffer.Load2(uvAddresses.x));
    float2 uvEdge1 = asfloat(vertexBuffer.Load2(uvAddresses.y)) - uv0;
    float2 uvEdge2 = asfloat(vertexBuffer.Load2(uvAddresses.z)) - uv0;
    float2 offset = displacedUV - uv0;

    // The point's barycentrics in the triangle's UV space. A degenerate UV triangle cuts nothing.
    float determinant = uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x;

    if(!(abs(determinant) > 1.0e-12f))
    {
        return false;
    }

    float weight1 = (offset.x * uvEdge2.y - offset.y * uvEdge2.x) / determinant;
    float weight2 = (uvEdge1.x * offset.y - uvEdge1.y * offset.x) / determinant;
    float weight0 = 1.0f - weight1 - weight2;

    // Each edge is crossed where the weight of the vertex opposite it turns negative.
    bool beyond01 = (edges & 1u) != 0u && weight2 < 0.0f;
    bool beyond12 = (edges & 2u) != 0u && weight0 < 0.0f;
    bool beyond20 = (edges & 4u) != 0u && weight1 < 0.0f;
    return beyond01 || beyond12 || beyond20;
}

#endif
