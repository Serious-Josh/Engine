#include "MaterialLighting.hlsli"
#include "NRD/NRD.hlsli"

// Matches the 160-byte Engine contract: matrices at 0/64, uint groups at 128/144.
struct NrdReflectionResolveConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 view;
    uint depthIndex;
    uint materialIndex;
    uint surfaceIndex;
    uint materialBufferIndex;
    uint nrdOutputIndex;
    uint outputIndex;
    uint width;
    uint height;
};

ConstantBuffer<NrdReflectionResolveConstants> constants : register(b0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(pixel.x >= constants.width || pixel.y >= constants.height)
    {
        return;
    }

    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[constants.outputIndex];
    // Match preparation's zero-radiance fallback for background/invalid primary data.
    outputTexture[pixel] = float4(0, 0, 0, 1);

    // Keep reconstruction, primary validity, and material factors identical to prepare.
    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    float depth = depthTexture.Load(int3(pixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return;
    }

    float2 uv = (float2(pixel) + 0.5f) / float2(constants.width, constants.height);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 worldPosition = mul(constants.inverseViewProjection, float4(ndc, depth, 1.0f));

    if(!all(isfinite(worldPosition)) || abs(worldPosition.w) < 1.0e-8f)
    {
        return;
    }

    worldPosition /= worldPosition.w;
    float3 viewPosition = mul(constants.view, worldPosition).xyz;

    if(!all(isfinite(viewPosition)) || viewPosition.z <= 0.0f)
    {
        return;
    }

    Texture2D<float4> surfaceTexture = ResourceDescriptorHeap[constants.surfaceIndex];
    Texture2D<float4> materialTexture = ResourceDescriptorHeap[constants.materialIndex];
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    float4 materialData = materialTexture.Load(int3(pixel, 0));

    if(!all(isfinite(surface)) || !all(isfinite(materialData)) || dot(surface.xyz, surface.xyz) < 1.0e-8f)
    {
        return;
    }

    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[constants.materialBufferIndex];
    uint materialCount, materialStride;
    materials.GetDimensions(materialCount, materialStride);
    float materialID = round(surface.w);

    if(materialID < 0.0f || materialID >= materialCount)
    {
        return;
    }

    GPUMaterial material = materials[(uint)materialID];
    float3 normal = normalize(surface.xyz);
    // Preserve the tracer's true mirror branch; do not square perceptual roughness.
    float roughness = material.roughness <= 0.001f ? 0.0f : max(saturate(materialData.a), 0.045f);

    // A rigid world-to-view rotation has transpose as its inverse. This gives
    // camera-to-surface direction in world space without another CB field.
    float3 V = normalize(mul(transpose((float3x3)constants.view), -viewPosition));
    float3 F0;

    if(material.model == 0)
    {
        F0 = lerp(float3(0.04f, 0.04f, 0.04f), materialData.rgb, saturate(material.metallic));
    }
    else if(material.model == 1)
    {
        float difference = material.ior - 1.0f;
        float sum = material.ior + 1.0f;
        F0 = (difference * difference / max(sum * sum, 1.0e-6f)).xxx;
    }
    else if(material.model == 2)
    {
        F0 = FresnelConductor(1.0f, material.conductorN, material.conductorK);
    }
    else
    {
        return;
    }

    float3 diffuseFactor, specularFactor;
    NRD_MaterialFactors(normal, V, materialData.rgb, F0, roughness, diffuseFactor, specularFactor);
    Texture2D<float4> nrdOutput = ResourceDescriptorHeap[constants.nrdOutputIndex];
    float4 decoded = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(nrdOutput.Load(int3(pixel, 0)));
    // Invert the exact safe divisor used by preparation. Decoded W is unused.
    float3 remodulatedRadiance = decoded.rgb * max(specularFactor, 1.0e-6f);
    outputTexture[pixel] = float4(remodulatedRadiance, 1.0f);
}
