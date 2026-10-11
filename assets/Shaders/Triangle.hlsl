#include "MaterialLighting.hlsli"

#define DEBUG_DISPLACEMENT_PATCH_LOD 1
#define DEBUG_DISPLACEMENT_TEST_LOD 1

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
    float4 currentClipPosition : TEXCOORD1;
    float4 previousClipPosition : TEXCOORD2;
    float2 viewDepths : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
};

struct PSOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
    float4 motion : SV_Target2;
    float4 geometricNormal : SV_Target3;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
    float roughness;
    uint materialIndex;
    uint materialBufferIndex;

    float3 baseColor;
    uint roughnessTextureIndex;
    uint displacementLODDescriptorIndex;
}

cbuffer CameraConstants : register(b1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 view;
    row_major float4x4 previousView;
}

cbuffer ObjectConstants : register(b2)
{
    row_major float4x4 model;
    row_major float4x4 previousModel;
    row_major float4x4 normalMatrix;
}

SamplerState sampler0 : register(s0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    float4 worldPosition =
        mul(
            model,
            float4(
                input.position,
                1.0f
            )
        );

    output.position =
        mul(
            viewProjection,
            worldPosition
        );

    output.currentClipPosition = output.position;

    float4 previousWorldPosition =
        mul(
            previousModel,
            float4(input.position, 1.0f)
        );

    output.previousClipPosition =
        mul(
            previousViewProjection,
            previousWorldPosition
        );

    output.viewDepths = float2(
        mul(view, worldPosition).z,
        mul(previousView, previousWorldPosition).z
    );

    output.worldNormal = mul((float3x3)normalMatrix, input.normal);
    output.worldTangent = float4(
        normalize(mul((float3x3)normalMatrix, input.tangent.xyz)),
        input.tangent.w
    );

    output.uv =
        input.uv;

    return output;
}

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    // Reuse the normal descriptor DWORD for access to the complete GPU material.
    StructuredBuffer<GPUMaterial> materials = ResourceDescriptorHeap[materialBufferIndex];
    GPUMaterial material = materials[materialIndex];
    uint normalTextureIndex = material.normalTextureIndex;

    Texture2D<float4> baseColorTexture =
        ResourceDescriptorHeap[
            textureIndex
        ];

    float3 sampledBaseColor =
        baseColorTexture.Sample(
            sampler0,
            input.uv
        ).rgb;

    float3 finalBaseColor =
        baseColor *
        sampledBaseColor;

#if DEBUG_DISPLACEMENT_PATCH_LOD
    // Temporary MirrorTestScene assumption; index 3 may identify unrelated materials in other scenes.
    if(materialIndex == 3u)
    {
        uint2 patchCoord = min(
            uint2(saturate(input.uv) * 8.0f),
            uint2(7u, 7u));
        uint patchIndex = patchCoord.x * 8u + patchCoord.y;
        ByteAddressBuffer patchLODs =
            ResourceDescriptorHeap[displacementLODDescriptorIndex];
        uint patchLOD = patchLODs.Load(patchIndex * 4u);
        switch(patchLOD)
        {
            case 0u: finalBaseColor = float3(0.1f, 0.9f, 0.2f); break;
            case 1u: finalBaseColor = float3(1.0f, 0.9f, 0.0f); break;
            case 2u: finalBaseColor = float3(1.0f, 0.5f, 0.0f); break;
            case 3u: finalBaseColor = float3(1.0f, 0.1f, 0.1f); break;
            case 4u: finalBaseColor = float3(0.6f, 0.2f, 1.0f); break;
            case 5u: finalBaseColor = float3(0.0f, 0.9f, 0.9f); break;
        }
    }
#endif

    bool useARM = material.armTextureIndex != 0xFFFFFFFFu && material.preferARM != 0u;
    float2 armRoughnessMetallic = float2(1.0f, 1.0f);
    float materialAO = 1.0f;
    if(useARM)
    {
        Texture2D<float4> armTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.armTextureIndex)];
        float3 sampledARM = armTexture.Sample(sampler0, input.uv).rgb;
        armRoughnessMetallic = sampledARM.gb;
        materialAO = saturate(sampledARM.r);
    }
    else if(material.aoTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> aoTexture = ResourceDescriptorHeap[
            NonUniformResourceIndex(material.aoTextureIndex)];
        materialAO = saturate(aoTexture.Sample(sampler0, input.uv).r);
    }

    float finalRoughness = roughness;
    if(useARM)
    {
        finalRoughness = saturate(roughness * armRoughnessMetallic.x);
    }
    else if(roughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> roughnessTexture = ResourceDescriptorHeap[roughnessTextureIndex];
        float sampledRoughness = roughnessTexture.Sample(sampler0, input.uv).r;
        finalRoughness = saturate(roughness * sampledRoughness);
    }

    output.material =
        float4(
            finalBaseColor,
            finalRoughness
        );

    float3 N = normalize(input.worldNormal);
    float3 mappedNormal = N;

    if(normalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalTextureIndex];
        float2 normalXY = normalTexture.Sample(sampler0, input.uv).rg * 2.0f - 1.0f;
        normalXY *= 0.5f;
        float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
        float3 sampledNormal = float3(normalXY, normalZ);
        float3 tangent = input.worldTangent.xyz - N * dot(N, input.worldTangent.xyz);
        float tangentLengthSquared = dot(tangent, tangent);
        float sampledLengthSquared = dot(sampledNormal, sampledNormal);
        float handedness = input.worldTangent.w;
        if(all(isfinite(N)) && all(isfinite(tangent)) &&
           isfinite(tangentLengthSquared) && tangentLengthSquared > 1.0e-8f &&
           all(isfinite(sampledNormal)) && isfinite(sampledLengthSquared) &&
           sampledLengthSquared > 1.0e-8f && isfinite(handedness) && abs(handedness) > 1.0e-8f)
        {
            float3 T = normalize(tangent);
            float3 B = cross(N, T) * handedness;
            float3 worldMappedNormal = T * sampledNormal.x + B * sampledNormal.y + N * sampledNormal.z;
            float mappedLengthSquared = dot(worldMappedNormal, worldMappedNormal);

            if(all(isfinite(B)) && all(isfinite(worldMappedNormal)) &&
               isfinite(mappedLengthSquared) && mappedLengthSquared > 1.0e-8f)
            {
                mappedNormal = normalize(worldMappedNormal);
            }
        }
    }

    output.surface = float4(mappedNormal, float(materialIndex));

    float effectiveMetallic = 0.0f;
    if(material.model == 0u)
    {
        effectiveMetallic = saturate(material.metallic);
        if(useARM)
        {
            effectiveMetallic = saturate(effectiveMetallic * armRoughnessMetallic.y);
        }
        else if(material.metallicTextureIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> metallicTexture = ResourceDescriptorHeap[
                NonUniformResourceIndex(material.metallicTextureIndex)];
            float sampledMetallic = metallicTexture.Sample(sampler0, input.uv).r;
            effectiveMetallic = saturate(effectiveMetallic * sampledMetallic);
        }
    }

    output.geometricNormal = float4(normalize(input.worldNormal), effectiveMetallic);

    output.motion = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if(all(isfinite(input.currentClipPosition)) &&
       all(isfinite(input.previousClipPosition)) &&
       all(isfinite(input.viewDepths)) &&
       abs(input.currentClipPosition.w) > 1.0e-6f &&
       abs(input.previousClipPosition.w) > 1.0e-6f)
    {
        float2 currentNDC =
            input.currentClipPosition.xy / input.currentClipPosition.w;
        float2 previousNDC =
            input.previousClipPosition.xy / input.previousClipPosition.w;

        float2 currentUV =
            currentNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        float2 previousUV =
            previousNDC * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

        float2 motion = currentUV - previousUV;

        float4 motionVector = float4(
            motion,
            input.viewDepths.y - input.viewDepths.x,
            0.0f
        );

        if(all(isfinite(motionVector)))
        {
            output.motion = motionVector;
        }
    }

    output.motion.a = materialAO;

    return output;
}
