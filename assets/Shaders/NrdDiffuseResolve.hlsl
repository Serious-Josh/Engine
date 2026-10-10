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
    uint depthIndex;
    uint normalIndex;
};

// Camera.h uses near=0.1 and far=1000; Matrix4::Perspective stores
// hardwareDepth = A + B / viewZ. Keep these synchronized with Camera.h.
float HardwareDepthToViewZ(float depth)
{
    const float nearPlane = 0.1f;
    const float farPlane = 1000.0f;
    return nearPlane * farPlane / (farPlane - depth * (farPlane - nearPlane));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(pixel.x >= width || pixel.y >= height)
    {
        return;
    }

    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[outputIndex];
    outputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalIndex];
    float depth = depthTexture.Load(int3(pixel, 0));
    float3 normal = normalTexture.Load(int3(pixel, 0)).rgb;
    float normalLengthSquared = dot(normal, normal);

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f ||
       !all(isfinite(normal)) || !isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-8f)
    {
        return;
    }

    // GBuffer surface normals are signed world-space values, without decoding.
    normal = normalize(normal);
    float centerViewZ = HardwareDepthToViewZ(depth);

    if(!isfinite(centerViewZ) || centerViewZ <= 0.0f || centerViewZ >= denoisingRange)
    {
        return;
    }

    uint2 halfResolution = uint2((width + 1u) / 2u, (height + 1u) / 2u);
    // Low-resolution texel i represents full-resolution pixel 2*i.
    float2 halfPosition = float2(pixel) * 0.5f;
    int2 basePixel = int2(floor(halfPosition));
    float2 fraction = frac(halfPosition);
    Texture2D<float> viewZTexture = ResourceDescriptorHeap[viewZIndex];
    Texture2D<float4> nrdOutput = ResourceDescriptorHeap[nrdOutputIndex];
    float3 accumulated = 0.0f;
    float totalWeight = 0.0f;
    float3 fallback = 0.0f;
    float nearestDistanceSquared = 1.0e30f;

    [unroll]
    for(uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for(uint x = 0u; x < 2u; ++x)
        {
            uint2 halfPixel = uint2(clamp(basePixel + int2(x, y), int2(0, 0), int2(halfResolution) - 1));
            uint2 representative = min(halfPixel * 2u, uint2(width, height) - 1u);
            float sampleDepth = depthTexture.Load(int3(representative, 0));
            float3 sampleNormal = normalTexture.Load(int3(representative, 0)).rgb;
            float sampleNormalLengthSquared = dot(sampleNormal, sampleNormal);
            float sampleViewZ = viewZTexture.Load(int3(halfPixel, 0));

            if(!isfinite(sampleDepth) || sampleDepth < 0.0f || sampleDepth >= 1.0f ||
               !all(isfinite(sampleNormal)) || !isfinite(sampleNormalLengthSquared) || sampleNormalLengthSquared < 1.0e-8f ||
               !isfinite(sampleViewZ) || sampleViewZ <= 0.0f || sampleViewZ >= denoisingRange)
            {
                continue;
            }

            float4 decoded = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(nrdOutput.Load(int3(halfPixel, 0)));

            if(!all(isfinite(decoded.rgb)))
            {
                continue;
            }

            float3 irradiance = min(max(decoded.rgb * PI, 0.0f), 65504.0f);

            if(!all(isfinite(irradiance)))
            {
                continue;
            }

            float2 delta = float2(halfPixel) - halfPosition;
            float distanceSquared = dot(delta, delta);
            if(distanceSquared < nearestDistanceSquared)
            {
                nearestDistanceSquared = distanceSquared;
                fallback = irradiance;
            }
            float spatialWeight = (x == 0u ? 1.0f - fraction.x : fraction.x) *
                                  (y == 0u ? 1.0f - fraction.y : fraction.y);
            // Relative view-space depth falloff (1% of center Z, minimum 1 cm).
            float depthWeight = exp(-abs(sampleViewZ - centerViewZ) / max(centerViewZ * 0.01f, 0.01f));
            float normalWeight = pow(saturate(dot(normal, normalize(sampleNormal))), 32.0f);
            float weight = spatialWeight * depthWeight * normalWeight;

            if(!isfinite(weight) || weight <= 0.0f)
            {
                continue;
            }

            accumulated += irradiance * weight;
            totalWeight += weight;
        }
    }
    float3 result = totalWeight > 1.0e-8f ? accumulated / totalWeight : fallback;

    if(all(isfinite(result)))
    {
        outputTexture[pixel] = float4(min(max(result, 0.0f), 65504.0f), 1.0f);
    }
}
