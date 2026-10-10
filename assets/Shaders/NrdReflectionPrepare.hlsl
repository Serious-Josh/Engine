#include "MaterialLighting.hlsli"

#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD/NRD.hlsli"

struct NrdReflectionPrepareConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 view;
    uint depthIndex;
    uint materialIndex;
    uint surfaceIndex;
    uint motionInputIndex;
    uint materialBufferIndex;
    uint rawReflectionIndex;
    uint trackingDistanceIndex;
    uint width;
    uint height;
    float hitDistanceA;
    float hitDistanceB;
    float hitDistanceC;
    float denoisingRange;
    uint viewZOutputIndex;
    uint motionOutputIndex;
    uint normalRoughnessOutputIndex;
    uint specRadianceHitDistOutputIndex;
    uint geometricNormalIndex;
    uint giHitDistanceIndex;
    uint diffRadianceHitDistOutputIndex;
};

ConstantBuffer<NrdReflectionPrepareConstants> constants : register(b0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;

    if(pixel.x >= constants.width || pixel.y >= constants.height)
    {
        return;
    }

    RWTexture2D<float> viewZOutput = ResourceDescriptorHeap[constants.viewZOutputIndex];
    RWTexture2D<float4> motionOutput = ResourceDescriptorHeap[constants.motionOutputIndex];
    RWTexture2D<float4> normalRoughnessOutput = ResourceDescriptorHeap[constants.normalRoughnessOutputIndex];
    RWTexture2D<float4> radianceOutput = ResourceDescriptorHeap[constants.specRadianceHitDistOutputIndex];

    // Finite defaults also cover invalid primary data. View Z is R32_FLOAT.
    viewZOutput[pixel] = max(constants.denoisingRange, 0.0f) * 2.0f + 1.0f;
    motionOutput[pixel] = 0.0f;
    normalRoughnessOutput[pixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0f, 0.0f);
    radianceOutput[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3(0, 0, 0), 0.0f, true);

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
        Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[constants.geometricNormalIndex];
        float effectiveMetallic = saturate(geometricNormalTexture.Load(int3(pixel, 0)).a);
        F0 = lerp(float3(0.04f, 0.04f, 0.04f), materialData.rgb, effectiveMetallic);
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

    Texture2D<float4> motionInput = ResourceDescriptorHeap[constants.motionInputIndex];
    float3 genericMotion = motionInput.Load(int3(pixel, 0)).xyz;
    if(all(isfinite(genericMotion)))
    {
        motionOutput[pixel] = float4(-genericMotion.xy, genericMotion.z, 0.0f);
    }

    float3 diffuseFactor, specularFactor;
    NRD_MaterialFactors(normal, V, materialData.rgb, F0, roughness, diffuseFactor, specularFactor);
    Texture2D<float4> rawReflection = ResourceDescriptorHeap[constants.rawReflectionIndex];
    // Keep the primary estimator's BRDF/MIS/PDF weights intact.
    float3 radiance = rawReflection.Load(int3(pixel, 0)).rgb / max(specularFactor, 1.0e-6f);

    Texture2D<float> trackingTexture = ResourceDescriptorHeap[constants.trackingDistanceIndex];
    float trackingDistance = trackingTexture.Load(int3(pixel, 0));
    float normalizedDistance = 0.0f; // -2 / invalid: REBLUR's no-data representation.

    if(isfinite(trackingDistance) && trackingDistance > 0.0f)
    {
        normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(trackingDistance, viewPosition.z,
            float3(constants.hitDistanceA, constants.hitDistanceB, constants.hitDistanceC), roughness);
    }
    else if(trackingDistance == -1.0f)
    {
        normalizedDistance = 1.0f; // Valid environment miss: end of the lobe.
    }

    viewZOutput[pixel] = viewPosition.z;
    normalRoughnessOutput[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0f);
    radianceOutput[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedDistance, true);
}
