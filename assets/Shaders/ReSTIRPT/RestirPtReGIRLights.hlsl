// M5.6 O0: the ReGIR light map for ReSTIR PT's ReGIR candidates. The shared ReGIR slots (ReGIRPresample.hlsl) hold RTXDI
// local light indices; ReSTIR PT needs, per local light, its scene-table index and its realized probability in the M4.1
// global power alias table (the p_1 the table-light NEE integrand keeps, exactly as a light-tile entry stores it).
// Output: uint2(table index, asuint(probability)) per RTXDI local index, then (M5.6 O0.2, from byte RESTIR_PT_REGIR_TABLE_P1
// = capacity x 8) asuint(probability) per scene-table index. Two dispatches with a UAV barrier between:
//   phase 0, one thread per table light: (table index, 0) at its local index (directional lights have none);
//   phase 1, one thread per global alias entry: the entry's own light's realized probability (each finite light with
//   power has exactly one entry; a local light without one keeps 0, which the path tree treats as an invalid candidate).

#include "../LightSampling.hlsli"

// ReSTIRPTPass::ReGIRLightCapacity x 8 bytes: the table-indexed probabilities follow the local-indexed map.
static const uint RESTIR_PT_REGIR_TABLE_P1 = 131072u * 8u;

// The light-tile constants' layout (ReSTIRPTLightTileConstants): tileCount, tileSize, frameSeed and padding.y unused,
// padding.x the phase.
cbuffer RestirPtLightTileConstants : register(b0)
{
    uint lightSamplingIndex;
    uint outputIndex;
    uint tileCount;
    uint tileSize;

    uint frameSeed;
    uint tableLightCount;
    uint phase;
    uint padding;
};

[numthreads(256, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const LightSamplingConstants sampling = LightSamplingLoadConstants(lightSamplingIndex);

    if(sampling.valid == 0u || sampling.tableLightCount != tableLightCount)
    {
        return;
    }

    ByteAddressBuffer structures = LightSamplingBuffer(sampling);
    RWByteAddressBuffer output = ResourceDescriptorHeap[outputIndex];
    // The table -> RTXDI local index translation (one uint per table light; its byte offset is the constants' padding).
    const uint translation = LIGHT_SAMPLING_HEADER_BYTES + sampling.padding;

    if(phase == 0u)
    {
        const uint light = thread.x;

        if(light < tableLightCount)
        {
            output.Store(RESTIR_PT_REGIR_TABLE_P1 + light * 4u, 0u);
            const uint local = structures.Load(translation + light * 4u);

            if(local < tableLightCount)
            {
                output.Store2(local * 8u, uint2(light, 0u));
            }
        }

        return;
    }

    const uint entryIndex = thread.x;

    if(entryIndex >= sampling.globalCount)
    {
        return;
    }

    // Alias entries are {threshold, alias entry, realized probability of this entry's light, light index}.
    const uint4 entry = structures.Load4(LIGHT_SAMPLING_HEADER_BYTES + sampling.globalOffset + entryIndex * 16u);

    if(entry.w < tableLightCount)
    {
        output.Store(RESTIR_PT_REGIR_TABLE_P1 + entry.w * 4u, entry.z);
        const uint local = structures.Load(translation + entry.w * 4u);

        if(local < tableLightCount)
        {
            output.Store(local * 8u + 4u, entry.z);
        }
    }
}
