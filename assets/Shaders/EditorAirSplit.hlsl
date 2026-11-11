// AIR.4D D3 Editor velocity vectors beside split cells (EditorAirVectors.hlsli): drawn indirectly, one instance per entry of the
// list CSClassify (EditorAir.hlsl) built, with the full D2 piece-aware sampling. Kept in its own file so that the lean pass never
// holds this code.

#define AIR_VECTOR_SPLIT 1
#include "EditorAirVectors.hlsli"

VectorVertex VSSplit(uint vertexIndex : SV_VertexID, uint entry : SV_InstanceID)
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    const uint instance = list.Load(entry * 4);
    return VectorVertexOf(AirVectorAt(field, parameters, instance, vertexIndex != 0, true), vertexIndex);
}
