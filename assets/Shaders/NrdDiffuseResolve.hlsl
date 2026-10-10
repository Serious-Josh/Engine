#include "Common.hlsli"
#include "NRD/NRD.hlsli"

cbuffer ResolveConstants : register(b0)
{
    uint nrdOutputIndex;
    uint outputIndex;
    uint width;
    uint height;

    uint viewZIndex;
    float denoisingRange;
    uint padding0;
    uint padding1;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    if(pixel.x >= width || pixel.y >= height)
        return;

    uint2 halfResolution = uint2((width + 1u) / 2u, (height + 1u) / 2u);
    uint2 halfPixel = min(pixel / 2u, halfResolution - 1u);

    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[outputIndex];
    outputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);

    Texture2D<float> viewZTexture = ResourceDescriptorHeap[viewZIndex];
    float viewZ = viewZTexture.Load(int3(halfPixel, 0));
    if(!isfinite(viewZ) || viewZ <= 0.0f || viewZ >= denoisingRange)
        return;

    Texture2D<float4> nrdOutput = ResourceDescriptorHeap[nrdOutputIndex];
    float4 decoded = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(nrdOutput.Load(int3(halfPixel, 0)));

    float3 irradiance = float3(0, 0, 0);
    if(all(isfinite(decoded.rgb)))
    {
        irradiance = max(decoded.rgb * PI, 0.0f);
        // Keep finite decoded values representable in the RGBA16_FLOAT output.
        irradiance = min(irradiance, 65504.0f);
    }
    outputTexture[pixel] = float4(irradiance, 1.0f);
}
