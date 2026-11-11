// AIR.4A/AIR.4B Editor velocity vectors: one line-list instance (2 vertices) per vector (AirVisualization.hlsli). Velocity is read
// from the published air field; nothing is read back to the CPU.
//
// AIR.4D D3: this is the lean pass (EditorAirVectors.hlsli): it draws every vector whose sample is plain and hides those beside a
// split cell, which CSClassify lists for the split pass (EditorAirSplit.hlsl). CSResetSplit empties the list first.

#include "EditorAirVectors.hlsli"

VectorVertex VSMain(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
{
    // Only the head needs the line walked to its (possibly wall-trimmed) end.
    return VectorVertexOf(AirVectorAt(field, parameters, instance, vertexIndex != 0, false), vertexIndex);
}

// One thread: the split pass's draw arguments, with no instances yet.
[numthreads(1, 1, 1)]
void CSResetSplit()
{
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    arguments.Store4(0, uint4(2u, 0u, 0u, 0u));
}

// One thread per instance: lists each vector the lean pass hides because its sample is beside a split cell.
[numthreads(64, 1, 1)]
void CSClassify(uint3 thread : SV_DispatchThreadID)
{
    const uint count = parameters.blocks.x * parameters.blocks.y * parameters.blocks.z * parameters.vectorsPerBlock;

    if(thread.x >= count || !AirVectorNeedsSplit(field, parameters, thread.x))
    {
        return;
    }

    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(splitArgumentsIndex)];
    RWByteAddressBuffer list = ResourceDescriptorHeap[NonUniformResourceIndex(splitListIndex)];
    uint entry = 0;
    arguments.InterlockedAdd(4, 1u, entry);
    list.Store(entry * 4, thread.x);
}
