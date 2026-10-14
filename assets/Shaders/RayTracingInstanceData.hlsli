#ifndef ENGINE_RAY_TRACING_INSTANCE_DATA
#define ENGINE_RAY_TRACING_INSTANCE_DATA

struct RayTracingInstanceData
{
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint materialIndex;
    uint vertexStride;

    uint normalOffset;
    uint uvOffset;
    uint indexed;
    uint tangentOffset;

    uint triangleSlotBufferIndex;
    uint triangleSlotOffset;
    uint slotMaterialBufferIndex;
    uint slotMaterialOffset;
};

uint ResolveHitMaterial(RayTracingInstanceData instance, uint primitiveIndex)
{
    uint slot = 0;

    if(instance.triangleSlotBufferIndex != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> triangleSlots = ResourceDescriptorHeap[
            NonUniformResourceIndex(instance.triangleSlotBufferIndex)];
        slot = triangleSlots[instance.triangleSlotOffset + primitiveIndex];
    }

    StructuredBuffer<uint> slotMaterials = ResourceDescriptorHeap[
        NonUniformResourceIndex(instance.slotMaterialBufferIndex)];
    return slotMaterials[instance.slotMaterialOffset + slot];
}

#endif
