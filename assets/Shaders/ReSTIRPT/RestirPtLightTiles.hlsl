// M5.1-2 (ReSTIR PT Enhanced, configuration B): presampled light tiles (Wyman and Panteleev 2021; paper section 6.1).
// Every frame, tileCount * tileSize entries are drawn independently from the M4.1 global power alias table of the finite
// table lights. An entry stores the light and the probability the alias draw realizes for it (LightSampling.hlsli), so a
// candidate picked uniformly from any tile has exactly that marginal probability: the RIS source density
// (RestirPtTableLightNee). Directional table lights are not in the global table; the path tree enumerates them.
// An entry without a valid draw holds light 0xFFFFFFFF (skipped by the path tree).

#include "../LightSampling.hlsli"

cbuffer RestirPtLightTileConstants : register(b0)
{
    uint lightSamplingIndex;
    uint outputIndex;
    uint tileCount;
    uint tileSize;

    uint frameSeed;
    uint tableLightCount;
    uint2 padding;
};

uint RestirPtTileHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

[numthreads(256, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint entry = thread.x;

    if(entry >= tileCount * tileSize)
    {
        return;
    }

    RWByteAddressBuffer tiles = ResourceDescriptorHeap[outputIndex];
    uint2 value = uint2(0xFFFFFFFFu, 0u);
    const LightSamplingConstants sampling = LightSamplingLoadConstants(lightSamplingIndex);

    if(sampling.valid != 0u && sampling.tableLightCount == tableLightCount && sampling.globalCount != 0u)
    {
        const uint random0 = RestirPtTileHash(entry ^ RestirPtTileHash(frameSeed ^ 0x4C545331u));
        const uint random1 = RestirPtTileHash(random0 ^ 0x68E31DA4u);
        const LightSamplingSample drawn = LightSamplingDrawGlobal(sampling, random0, random1);

        if(drawn.valid && drawn.probability > 0.0f)
        {
            value = uint2(drawn.light, asuint(drawn.probability));
        }
    }

    tiles.Store2(entry * 8u, value);
}
