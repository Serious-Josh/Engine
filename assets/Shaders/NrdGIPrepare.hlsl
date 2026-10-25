#include "MaterialLighting.hlsli"

#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD/NRD.hlsli"

struct NrdGIPrepareConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 view;

    uint depthIndex;
    uint materialIndex;
    uint surfaceIndex;
    uint motionInputIndex;

    uint materialBufferIndex;
    uint rawGIIndex;
    uint giHitDistanceIndex;
    uint viewZOutputIndex;

    uint motionOutputIndex;
    uint normalRoughnessOutputIndex;
    uint diffRadianceHitDistOutputIndex;
    uint fullWidth;

    uint fullHeight;
    uint giWidth;
    uint giHeight;
    uint padding;

    float hitDistanceA;
    float hitDistanceB;
    float hitDistanceC;
    float denoisingRange;
};

ConstantBuffer<NrdGIPrepareConstants> constants : register(b0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 giPixel = dispatchThreadID.xy;

    if(giPixel.x >= constants.giWidth || giPixel.y >= constants.giHeight)
    {
        return;
    }

    RWTexture2D<float> viewZOutput = ResourceDescriptorHeap[constants.viewZOutputIndex];
    RWTexture2D<float4> motionOutput = ResourceDescriptorHeap[constants.motionOutputIndex];
    RWTexture2D<float4> normalRoughnessOutput = ResourceDescriptorHeap[constants.normalRoughnessOutputIndex];

    RWTexture2D<float4> diffuseOutput = ResourceDescriptorHeap[constants.diffRadianceHitDistOutputIndex];

    // Finite defaults also cover invalid primary data. View Z is R32_FLOAT.
    viewZOutput[giPixel] = max(constants.denoisingRange, 0.0f) * 2.0f + 1.0f;
    motionOutput[giPixel] = 0.0f;
    normalRoughnessOutput[giPixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0f, 0.0f);

    diffuseOutput[giPixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3(0, 0, 0), 0.0f, true);

    if(constants.fullWidth == 0u || constants.fullHeight == 0u)
    {
        return;
    }

    uint2 gbufferPixel = min(
        giPixel * 2u,
        uint2(constants.fullWidth, constants.fullHeight) - 1u);

    Texture2D<float> depthTexture = ResourceDescriptorHeap[constants.depthIndex];
    float depth = depthTexture.Load(int3(gbufferPixel, 0));

    if(!isfinite(depth) || depth < 0.0f || depth >= 1.0f)
    {
        return;
    }

    float2 uv = (float2(gbufferPixel) + 0.5f) / float2(constants.fullWidth, constants.fullHeight);
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
    float4 surface = surfaceTexture.Load(int3(gbufferPixel, 0));
    float4 materialData = materialTexture.Load(int3(gbufferPixel, 0));

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
    // Preserve the tracer's true mirror branch (RayTracedReflection.hlsl: effective G-buffer roughness <= 0.001); do not
    // square perceptual roughness.
    float effectiveRoughness = saturate(materialData.a);
    float roughness = effectiveRoughness <= 0.001f ? 0.0f : max(effectiveRoughness, 0.045f);

    if(material.model > 2u)
    {
        return;
    }

    Texture2D<float4> motionInput = ResourceDescriptorHeap[constants.motionInputIndex];
    float3 genericMotion = motionInput.Load(int3(gbufferPixel, 0)).xyz;
    if(all(isfinite(genericMotion)))
    {
        motionOutput[giPixel] = float4(-genericMotion.xy, genericMotion.z, 0.0f);
    }

    viewZOutput[giPixel] = viewPosition.z;
    normalRoughnessOutput[giPixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0f);

    Texture2D<float4> rawGI = ResourceDescriptorHeap[constants.rawGIIndex];
    Texture2D<float> giHitDistanceTexture = ResourceDescriptorHeap[constants.giHitDistanceIndex];
    float giHitDistance = giHitDistanceTexture.Load(int3(giPixel, 0));
    float diffuseNormalizedDistance = 0.0f;

    if(isfinite(giHitDistance) && giHitDistance > 0.0f)
    {
        diffuseNormalizedDistance = REBLUR_FrontEnd_GetNormHitDist(giHitDistance, viewPosition.z,
            float3(constants.hitDistanceA, constants.hitDistanceB, constants.hitDistanceC), 1.0f);
    }
    else if(giHitDistance == -1.0f)
    {
        diffuseNormalizedDistance = 1.0f;
    }
    else
    {
        return; // Invalid GI leaves the diffuse no-data default; shared guides stay unchanged.
    }

    // Complete irradiance excludes primary albedo; historical bundles also include their baseline.
    float3 diffuseRadiance = rawGI.Load(int3(giPixel, 0)).rgb / PI;
    diffuseOutput[giPixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuseRadiance, diffuseNormalizedDistance, true);
}
