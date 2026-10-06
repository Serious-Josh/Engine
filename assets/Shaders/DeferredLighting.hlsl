#include "MaterialLighting.hlsli"

static const float RT_REFLECTION_ROUGHNESS_FADE_START = 0.30f;
static const float RT_REFLECTION_ROUGHNESS_FADE_END = 0.40f;

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer GBufferConstants : register(b0)
{
    uint materialIndex;
    uint surfaceIndex;
    uint depthIndex;
    uint materialBufferIndex;
    uint environmentIndex;
    uint environmentCubemapIndex;
    uint diffuseIrradianceIndex;
    uint brdfIntegrationLUTIndex;
    uint ggxAverageAlbedoLUTIndex;
    uint ggxMultiscatterIrradianceIndex;
    uint shadowVisibilityIndex;
    uint reflectionIndex;
}

cbuffer ReconstructionConstants : register(b1)
{
    row_major float4x4 inverseViewProjection;

    float3 cameraPosition;
    float padding;
}

cbuffer DirectionalLightConstants : register(b2)
{
    float3 lightDirection;
    float lightIntensity;

    float3 lightColor;
    float ambientStrength;
}

SamplerState sampler0 : register(s0);

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };

    float2 uvs[3] =
    {
        float2(0.0f,  1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f,  1.0f)
    };

    output.position =
        float4(
            positions[vertexID],
            0.0f,
            1.0f
        );

    output.uv =
        uvs[vertexID];

    return output;
}

float3 ReconstructWorldDirection(float2 uv)
{
    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            1.0f,
            1.0f
        );

    float4 worldPosition =
        mul(
            inverseViewProjection,
            clipPosition
        );

    worldPosition /=
        worldPosition.w;

    return normalize(
        worldPosition.xyz -
        cameraPosition
    );
}

float4 PSMain(VSOutput input) : SV_Target0
{
    Texture2D<float4> materialTexture =
        ResourceDescriptorHeap[
            materialIndex
        ];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            surfaceIndex
        ];

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            depthIndex
        ];

    Texture2D<float4> environmentTexture =
        ResourceDescriptorHeap[
            environmentIndex
        ];

    TextureCube<float4> environmentCubemap =
        ResourceDescriptorHeap[
            environmentCubemapIndex
        ];

    TextureCube<float4> diffuseIrradiance =
        ResourceDescriptorHeap[
            diffuseIrradianceIndex
        ];

    Texture2D<float2> brdfIntegrationLUT =
        ResourceDescriptorHeap[
            brdfIntegrationLUTIndex
        ];

    Texture2D<float> ggxAverageAlbedoLUT =
        ResourceDescriptorHeap[
            ggxAverageAlbedoLUTIndex
        ];

    TextureCube<float4> ggxMultiscatterIrradiance =
        ResourceDescriptorHeap[
            ggxMultiscatterIrradianceIndex
        ];

    Texture2D<float> shadowVisibilityTexture =
        ResourceDescriptorHeap[
            shadowVisibilityIndex
        ];

    Texture2D<float4> reflectionTexture =
        ResourceDescriptorHeap[
            reflectionIndex
        ];

    int2 pixel =
        int2(input.position.xy);


    float depth =
        depthTexture.Load(
            int3(
                pixel,
                0
            )
        );

    if(depth >= 1.0f)
    {
        float3 worldDirection =
            ReconstructWorldDirection(
                input.uv
            );

        float2 environmentUV =
            DirectionToEquirectangularUV(
                worldDirection
            );

        float3 environmentColor =
            environmentTexture.SampleLevel(
                sampler0,
                environmentUV,
                0.0f
            ).rgb;

        return float4(
            environmentColor,
            1.0f
        );
    }

    float shadowVisibility =
        shadowVisibilityTexture.Load(
            int3(
                pixel,
                0
            )
        );

    float2 ndc;

    ndc.x =
        input.uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - input.uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            depth,
            1.0f
        );

    float4 worldPosition =
        mul(
            inverseViewProjection,
            clipPosition
        );

    worldPosition /=
        worldPosition.w;

    float4 materialData =
        materialTexture.Load(
            int3(
                pixel,
                0
            )
        );

    float3 baseColor =
        materialData.rgb;

    float4 surfaceData =
        surfaceTexture.Load(
            int3(
                pixel,
                0
            )
        );

    float3 normal =
        normalize(
            surfaceData.rgb
        );

    uint gpuMaterialIndex =
        (uint)round(
            surfaceData.a
        );

    StructuredBuffer<GPUMaterial> materialBuffer =
        ResourceDescriptorHeap[
            materialBufferIndex
        ];

    GPUMaterial gpuMaterial =
        materialBuffer[
            gpuMaterialIndex
        ];

    float roughness =
        gpuMaterial.roughness;

    float3 lightDirectionToSurface =
        lightDirection;

    float3 directionToLight =
        -lightDirectionToSurface;

    float3 viewDirection =
        normalize(
            cameraPosition -
            worldPosition.xyz
        );

    float NdotL =
        saturate(
            dot(
                normal,
                directionToLight
            )
        );

    float NdotV =
        saturate(
            dot(
                normal,
                viewDirection
            )
        );

    float3 directBRDF =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    float3 environmentDiffuse =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    float3 environmentSpecular =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    float3 rayTracedSpecular =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    float rtWeight =
        1.0f - smoothstep(
            RT_REFLECTION_ROUGHNESS_FADE_START,
            RT_REFLECTION_ROUGHNESS_FADE_END,
            roughness
        );

    float3 finalSpecular = float3(0.0f, 0.0f, 0.0f);

    float3 reflectionRadiance =
        reflectionTexture.Load(
            int3(
                pixel,
                0
            )
        ).rgb;

    if(gpuMaterial.model == 0)
    {
        float metallic =
            saturate(
                gpuMaterial.metallic
            );

        float3 dielectricF0 =
            float3(
                0.04f,
                0.04f,
                0.04f
            );

        float3 F0 =
            lerp(
                dielectricF0,
                baseColor,
                metallic
            );

        float3 diffuseColor =
            baseColor *
            (1.0f - metallic);

        float directDirectionalAlbedoLight =
            0.0f;

        float directDirectionalAlbedoView =
            0.0f;

        float directAverageAlbedo =
            0.0f;

        if(NdotL > 0.0f && NdotV > 0.0f)
        {
            float2 directionalAlbedoLightData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotL,
                        roughness
                    ),
                    0.0f
                ).rg;

            float2 directionalAlbedoViewData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotV,
                        roughness
                    ),
                    0.0f
                ).rg;

            directDirectionalAlbedoLight =
                directionalAlbedoLightData.x +
                directionalAlbedoLightData.y;

            directDirectionalAlbedoView =
                directionalAlbedoViewData.x +
                directionalAlbedoViewData.y;

            directAverageAlbedo =
                ggxAverageAlbedoLUT.SampleLevel(
                    sampler0,
                    float2(
                        roughness,
                        0.5f
                    ),
                    0.0f
                ).r;
        }

        directBRDF =
            EvaluateMetallicRoughnessDirectBRDF(
                gpuMaterial,
                baseColor,
                normal,
                directionToLight,
                viewDirection,
                directDirectionalAlbedoLight,
                directDirectionalAlbedoView,
                directAverageAlbedo
            );

        float3 reflectionDirection =
            reflect(
                -viewDirection,
                normal
            );

        float maxEnvironmentMip =
            10.0f;

        float environmentMip =
            roughness *
            maxEnvironmentMip;

        float3 prefilteredEnvironment =
            environmentCubemap.SampleLevel(
                sampler0,
                reflectionDirection,
                environmentMip
            ).rgb;

        float2 integratedBRDF =
            brdfIntegrationLUT.SampleLevel(
                sampler0,
                float2(
                    NdotV,
                    roughness
                ),
                0.0f
            ).rg;

        environmentSpecular =
            prefilteredEnvironment *
            (
                F0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float directionalAlbedoView =
            saturate(
                integratedBRDF.x +
                integratedBRDF.y
            );

        float averageAlbedo =
            ggxAverageAlbedoLUT.SampleLevel(
                sampler0,
                float2(
                    roughness,
                    0.5f
                ),
                0.0f
            ).r;

        float3 averageFresnel =
            AverageSchlickFresnel(F0);

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                averageAlbedo,
                averageFresnel
            );

        float multiscatterMip =
            roughness * 6.0f;

        float3 multiscatterIrradiance =
            ggxMultiscatterIrradiance.SampleLevel(
                sampler0,
                normal,
                multiscatterMip
            ).rgb;

        float multiscatterNormalization =
            max(
                1.0f - averageAlbedo,
                1.0e-6f
            );

        float3 environmentMultipleScatter =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalAlbedoView) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular +=
            environmentMultipleScatter;

        rayTracedSpecular =
            reflectionRadiance;

        finalSpecular =
            lerp(environmentSpecular, rayTracedSpecular, rtWeight);

        float3 irradiance =
            diffuseIrradiance.SampleLevel(
                sampler0,
                normal,
                0.0f
            ).rgb;

        float3 environmentFresnel =
            F0 +
            (1.0f - F0) *
            pow(
                1.0f - NdotV,
                5.0f
            );

        float3 environmentDiffuseWeight =
            1.0f -
            environmentFresnel;

        environmentDiffuse =
            environmentDiffuseWeight *
            (diffuseColor / PI) *
            irradiance;
    }
    else if(gpuMaterial.model == 1)
    {
        float iorDifference =
            gpuMaterial.ior - 1.0f;

        float iorSum =
            gpuMaterial.ior + 1.0f;

        float dielectricF0 =
            (iorDifference * iorDifference) /
            (iorSum * iorSum);

        float3 F0 =
            float3(
                dielectricF0,
                dielectricF0,
                dielectricF0
            );

        float directDirectionalAlbedoLight =
            0.0f;

        float directDirectionalAlbedoView =
            0.0f;

        float directAverageAlbedo =
            0.0f;

        if(NdotL > 0.0f && NdotV > 0.0f)
        {
            float2 directionalAlbedoLightData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotL,
                        roughness
                    ),
                    0.0f
                ).rg;

            float2 directionalAlbedoViewData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotV,
                        roughness
                    ),
                    0.0f
                ).rg;

            directDirectionalAlbedoLight =
                directionalAlbedoLightData.x +
                directionalAlbedoLightData.y;

            directDirectionalAlbedoView =
                directionalAlbedoViewData.x +
                directionalAlbedoViewData.y;

            directAverageAlbedo =
                ggxAverageAlbedoLUT.SampleLevel(
                    sampler0,
                    float2(
                        roughness,
                        0.5f
                    ),
                    0.0f
                ).r;
        }

        directBRDF =
            EvaluateDielectricDirectBRDF(
                gpuMaterial,
                baseColor,
                normal,
                directionToLight,
                viewDirection,
                directDirectionalAlbedoLight,
                directDirectionalAlbedoView,
                directAverageAlbedo
            );

        float3 reflectionDirection =
            reflect(
                -viewDirection,
                normal
            );

        float maxEnvironmentMip =
            10.0f;

        float environmentMip =
            roughness *
            maxEnvironmentMip;

        float3 prefilteredEnvironment =
            environmentCubemap.SampleLevel(
                sampler0,
                reflectionDirection,
                environmentMip
            ).rgb;

        float2 integratedBRDF =
            brdfIntegrationLUT.SampleLevel(
                sampler0,
                float2(
                    NdotV,
                    roughness
                ),
                0.0f
            ).rg;

        environmentSpecular =
            prefilteredEnvironment *
            (
                F0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float directionalAlbedoView =
            saturate(
                integratedBRDF.x +
                integratedBRDF.y
            );

        float averageAlbedo =
            ggxAverageAlbedoLUT.SampleLevel(
                sampler0,
                float2(
                    roughness,
                    0.5f
                ),
                0.0f
            ).r;

        float3 averageFresnel =
            AverageSchlickFresnel(F0);

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                averageAlbedo,
                averageFresnel
            );

        float multiscatterMip =
            roughness * 6.0f;

        float3 multiscatterIrradiance =
            ggxMultiscatterIrradiance.SampleLevel(
                sampler0,
                normal,
                multiscatterMip
            ).rgb;

        float multiscatterNormalization =
            max(
                1.0f - averageAlbedo,
                1.0e-6f
            );

        float3 environmentMultipleScatter =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalAlbedoView) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular +=
            environmentMultipleScatter;

        rayTracedSpecular =
            reflectionRadiance;

        finalSpecular =
            lerp(environmentSpecular, rayTracedSpecular, rtWeight);

        float3 irradiance =
            diffuseIrradiance.SampleLevel(
                sampler0,
                normal,
                0.0f
            ).rgb;

        float3 environmentFresnel =
            F0 +
            (1.0f - F0) *
            pow(
                1.0f - NdotV,
                5.0f
            );

        float3 environmentDiffuseWeight =
            1.0f -
            environmentFresnel;

        environmentDiffuse =
            environmentDiffuseWeight *
            (baseColor / PI) *
            irradiance;
    }
    else if(gpuMaterial.model == 2)
    {
        float directDirectionalAlbedoLight =
            0.0f;

        float directDirectionalAlbedoView =
            0.0f;

        float directAverageAlbedo =
            0.0f;

        if(NdotL > 0.0f && NdotV > 0.0f)
        {
            float2 directionalAlbedoLightData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotL,
                        roughness
                    ),
                    0.0f
                ).rg;

            float2 directionalAlbedoViewData =
                brdfIntegrationLUT.SampleLevel(
                    sampler0,
                    float2(
                        NdotV,
                        roughness
                    ),
                    0.0f
                ).rg;

            directDirectionalAlbedoLight =
                directionalAlbedoLightData.x +
                directionalAlbedoLightData.y;

            directDirectionalAlbedoView =
                directionalAlbedoViewData.x +
                directionalAlbedoViewData.y;

            directAverageAlbedo =
                ggxAverageAlbedoLUT.SampleLevel(
                    sampler0,
                    float2(
                        roughness,
                        0.5f
                    ),
                    0.0f
                ).r;
        }

        directBRDF =
            EvaluateConductorDirectBRDF(
                gpuMaterial,
                normal,
                directionToLight,
                viewDirection,
                directDirectionalAlbedoLight,
                directDirectionalAlbedoView,
                directAverageAlbedo
            );

        float3 reflectionDirection =
            reflect(
                -viewDirection,
                normal
            );

        float maxEnvironmentMip =
            10.0f;

        float environmentMip =
            roughness *
            maxEnvironmentMip;

        float3 prefilteredEnvironment =
            environmentCubemap.SampleLevel(
                sampler0,
                reflectionDirection,
                environmentMip
            ).rgb;

        float2 integratedBRDF =
            brdfIntegrationLUT.SampleLevel(
                sampler0,
                float2(
                    NdotV,
                    roughness
                ),
                0.0f
            ).rg;

        float3 conductorF0 =
            FresnelConductor(
                1.0f,
                gpuMaterial.conductorN,
                gpuMaterial.conductorK
            );

        environmentSpecular =
            prefilteredEnvironment *
            (
                conductorF0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float directionalAlbedoView =
            saturate(
                integratedBRDF.x +
                integratedBRDF.y
            );

        float averageAlbedo =
            ggxAverageAlbedoLUT.SampleLevel(
                sampler0,
                float2(
                    roughness,
                    0.5f
                ),
                0.0f
            ).r;

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                averageAlbedo,
                gpuMaterial.averageConductorFresnel
            );

        float multiscatterMip =
            roughness * 6.0f;

        float3 multiscatterIrradiance =
            ggxMultiscatterIrradiance.SampleLevel(
                sampler0,
                normal,
                multiscatterMip
            ).rgb;

        float multiscatterNormalization =
            max(
                1.0f - averageAlbedo,
                1.0e-6f
            );

        float3 environmentMultipleScatter =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalAlbedoView) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular +=
            environmentMultipleScatter;

        rayTracedSpecular =
            reflectionRadiance;

        finalSpecular =
            lerp(environmentSpecular, rayTracedSpecular, rtWeight);
    }

    float3 directional =
        directBRDF *
        lightColor *
        lightIntensity *
        NdotL *
        shadowVisibility;

    float3 lighting =
        directional +
        environmentDiffuse +
        finalSpecular;

    return float4(
        lighting,
        1.0f
    );
}
