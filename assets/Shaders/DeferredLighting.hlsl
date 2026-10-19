#include "MaterialLighting.hlsli"
#include "ShadingNormal.hlsli"

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
    uint giIrradianceIndex;
    uint enableRTGI;
    uint aoVisibilityIndex;
    uint lightingDebugView;
    float rtgiContactAOStrength;
    uint enableShadingNormalCorrection;
}

cbuffer ReconstructionConstants : register(b1)
{
    row_major float4x4 inverseViewProjection;

    float3 cameraPosition;
    uint geometricNormalIndex;
    uint motionIndex;
    uint3 reconstructionPadding;
}

cbuffer DirectionalLightConstants : register(b2)
{
    float3 lightDirection;
    float lightIntensity;

    float3 lightColor;
    float ambientStrength;
}

#include "SceneLights.hlsli"

cbuffer SceneLightConstants : register(b3)
{
    uint sceneLightCount;
    uint3 sceneLightPadding;

    GPULight sceneLights[SCENE_LIGHT_CAPACITY];
}

SamplerState sampler0 : register(s0);
SamplerState diagnosticEnvironmentSampler : register(s1);

// The material's direct BRDF for one light direction, using exactly the sun path's LUT inputs and
// per-model evaluation, so table Lights and the sun shade identically.
float3 EvaluateMaterialDirectBRDF(
    GPUMaterial gpuMaterial,
    float3 baseColor,
    float3 normal,
    float3 directionToLight,
    float3 viewDirection,
    float roughness,
    float NdotV,
    Texture2D<float2> brdfIntegrationLUT,
    Texture2D<float> ggxAverageAlbedoLUT)
{
    float NdotL = saturate(dot(normal, directionToLight));
    float directionalAlbedoLight = 0.0f;
    float directionalAlbedoView = 0.0f;
    float averageAlbedo = 0.0f;

    if(NdotL > 0.0f && NdotV > 0.0f)
    {
        float2 lightData = brdfIntegrationLUT.SampleLevel(sampler0, float2(NdotL, roughness), 0.0f).rg;
        float2 viewData = brdfIntegrationLUT.SampleLevel(sampler0, float2(NdotV, roughness), 0.0f).rg;
        directionalAlbedoLight = lightData.x + lightData.y;
        directionalAlbedoView = viewData.x + viewData.y;
        averageAlbedo = ggxAverageAlbedoLUT.SampleLevel(sampler0, float2(roughness, 0.5f), 0.0f).r;
    }

    if(gpuMaterial.model == 0u)
    {
        return EvaluateMetallicRoughnessDirectBRDF(gpuMaterial, baseColor, normal, directionToLight, viewDirection,
            directionalAlbedoLight, directionalAlbedoView, averageAlbedo);
    }
    else if(gpuMaterial.model == 1u)
    {
        return EvaluateDielectricDirectBRDF(gpuMaterial, baseColor, normal, directionToLight, viewDirection,
            directionalAlbedoLight, directionalAlbedoView, averageAlbedo);
    }
    else if(gpuMaterial.model == 2u)
    {
        return EvaluateConductorDirectBRDF(gpuMaterial, normal, directionToLight, viewDirection,
            directionalAlbedoLight, directionalAlbedoView, averageAlbedo);
    }

    return float3(0.0f, 0.0f, 0.0f);
}

float3 IntegrateDiagnosticHDRIFrame(float3 normal, float3 tangent, float3 bitangent, uint sampleCount, float sourceMip = 0.0f)
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[environmentIndex];
    float3 sum = 0.0f;
    [loop]
    for(uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = float2(float(i) / float(sampleCount),
            float(reversebits(i)) * 2.3283064365386963e-10f);
        float radius = sqrt(xi.x);
        float phi = 2.0f * PI * xi.y;
        float3 localDirection = float3(radius * cos(phi), radius * sin(phi),
            sqrt(max(0.0f, 1.0f - xi.x)));
        float3 direction = normalize(tangent * localDirection.x +
            bitangent * localDirection.y + normal * localDirection.z);
        float3 radiance = sourceTexture.SampleLevel(diagnosticEnvironmentSampler,
            DirectionToEquirectangularUV(direction), sourceMip).rgb;

        if(!all(isfinite(radiance)))
        {
            return radiance;
        }

        sum += radiance;
    }
    return sum * (PI / float(sampleCount));
}

float3 IntegrateDiagnosticHDRIIrradiance(float3 normal, uint sampleCount, bool rotateFrame, float sourceMip = 0.0f)
{
    float3 up = abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    if(rotateFrame)
    {
        float3 originalTangent = tangent;
        tangent = bitangent;
        bitangent = -originalTangent;
    }
    return IntegrateDiagnosticHDRIFrame(normal, tangent, bitangent, sampleCount, sourceMip);
}

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

    const bool sourceMipDebug = lightingDebugView >= 28u && lightingDebugView <= 30u;
    const bool lightingDebug = lightingDebugView >= 19u && lightingDebugView <= 30u;

    if(lightingDebug && (!isfinite(depth) || depth < 0.0f || depth >= 1.0f))
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    if(lightingDebugView == 32u && (!isfinite(depth) || depth < 0.0f || depth >= 1.0f))
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

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

    if(lightingDebug && !all(isfinite(worldPosition)))
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // Reserved for material AO composition; leave lighting unchanged in this step.
    Texture2D<float4> motionTexture = ResourceDescriptorHeap[motionIndex];
    float materialAO = saturate(motionTexture.Load(int3(pixel, 0)).a);

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

    if(lightingDebug && (!all(isfinite(surfaceData.rgb)) || dot(surfaceData.rgb, surfaceData.rgb) < 1.0e-8f))
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

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

    if(gpuMaterial.model == 0u)
    {
        Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[geometricNormalIndex];
        gpuMaterial.metallic = saturate(geometricNormalTexture.Load(int3(pixel, 0)).a);
    }

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

    // Development-only mode 32 is gated by the CPU debug-view plumbing.
    if(lightingDebugView == 32u)
    {
        Texture2D<float4> geometricNormalTexture = ResourceDescriptorHeap[geometricNormalIndex];
        float3 geometricNormal = geometricNormalTexture.Load(int3(pixel, 0)).rgb;
        float geometricNormalLengthSquared = dot(geometricNormal, geometricNormal);
        float shadingNormalLengthSquared = dot(surfaceData.rgb, surfaceData.rgb);

        if(!all(isfinite(geometricNormal)) || !isfinite(geometricNormalLengthSquared) ||
           geometricNormalLengthSquared < 1.0e-8f || !all(isfinite(surfaceData.rgb)) ||
           !isfinite(shadingNormalLengthSquared) || shadingNormalLengthSquared < 1.0e-8f ||
           !all(isfinite(worldPosition)) || !all(isfinite(viewDirection)))
        {
            return float4(1.0f, 1.0f, 0.0f, 1.0f);
        }

        geometricNormal *= rsqrt(geometricNormalLengthSquared);
        float geometricNormalDotView = dot(geometricNormal, viewDirection);
        float shadingNormalDotView = dot(normal, viewDirection);
        bool crossesViewHorizon = geometricNormalDotView > 0.0f && shadingNormalDotView <= 0.0f;

        return float4(crossesViewHorizon ? float3(1.0f, 0.0f, 1.0f) : float3(0.0f, 0.0f, 0.0f), 1.0f);
    }

    if(enableShadingNormalCorrection != 0u)
    {
        Texture2D<float4> referenceNormalTexture = ResourceDescriptorHeap[geometricNormalIndex];
        float3 referenceNormal = referenceNormalTexture.Load(int3(pixel, 0)).rgb;
        float referenceLengthSquared = dot(referenceNormal, referenceNormal);

        if(all(isfinite(referenceNormal)) && isfinite(referenceLengthSquared) && referenceLengthSquared > 1.0e-8f)
        {
            referenceNormal *= rsqrt(referenceLengthSquared);
            normal = CorrectShadingNormalViewHorizon(normal, referenceNormal, viewDirection);
        }
    }

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

    float3 diagnosticDirectIrradiance = 0.0f;
    [branch]
    if(sourceMipDebug)
    {
        uint requestedMip = lightingDebugView == 28u ? 0u : (lightingDebugView == 29u ? 4u : 6u);
        uint sourceWidth, sourceHeight, sourceMipCount;
        environmentTexture.GetDimensions(0u, sourceWidth, sourceHeight, sourceMipCount);

        if(requestedMip >= sourceMipCount)
        {
            return float4(1.0f, 0.0f, 1.0f, 1.0f); // Never silently clamp an unavailable mip.
        }

        diagnosticDirectIrradiance = IntegrateDiagnosticHDRIIrradiance(normal, 4096u, false, float(requestedMip));

        if(!all(isfinite(diagnosticDirectIrradiance)))
        {
            return float4(diagnosticDirectIrradiance, 1.0f);
        }
    }
    [branch]
    if(lightingDebugView == 26u || lightingDebugView == 27u)
    {
        // Build once and derive every orientation, including the single reference.
        float3 up = abs(normal.y) < 0.999f
            ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        float3 tangent = normalize(cross(up, normal));
        float3 bitangent = cross(normal, tangent);
        const float diagonal = 0.7071067811865475f;
        const float2 rotations[4] = {
            float2(1.0f, 0.0f), float2(diagonal, diagonal),
            float2(0.0f, 1.0f), float2(-diagonal, diagonal)
        };
        float3 multi = 0.0f;
        [unroll]
        for(uint orientation = 0u; orientation < 4u; ++orientation)
        {
            float c = rotations[orientation].x;
            float s = rotations[orientation].y;
            float3 rotatedTangent = c * tangent + s * bitangent;
            float3 rotatedBitangent = -s * tangent + c * bitangent;
            float3 estimate = IntegrateDiagnosticHDRIFrame(normal, rotatedTangent, rotatedBitangent, 1024u);

            if(!all(isfinite(estimate)))
            {
                return float4(estimate, 1.0f);
            }

            multi += estimate;
        }
        // Each estimate already includes PI/1024; averaging yields PI/4096 overall.
        multi *= 0.25f;

        if(!all(isfinite(multi)))
        {
            return float4(multi, 1.0f);
        }

        if(lightingDebugView == 27u)
        {
            float3 single = IntegrateDiagnosticHDRIFrame(normal, tangent, bitangent, 4096u);
            float3 residual = multi - single;

            if(!all(isfinite(residual)))
            {
                return float4(1.0f, 0.0f, 1.0f, 1.0f);
            }

            const float residualScale = 10.0f;
            float positive = saturate(max(residual.r, max(residual.g, residual.b)) / residualScale);
            float negative = saturate(-min(residual.r, min(residual.g, residual.b)) / residualScale);
            return float4(0.5f * (1.0f - max(positive, negative)) +
                float3(positive, 0.0f, negative) / (1.0f + min(positive, negative)), 1.0f);
        }
        diagnosticDirectIrradiance = multi;
    }
    [branch]
    if(lightingDebugView == 23u || lightingDebugView == 24u || lightingDebugView == 25u)
    {
        // View 23 convergence experiment; retain View 24's 4096-sample comparison.
        uint referenceSampleCount = lightingDebugView == 23u ? 16384u : 4096u;
        diagnosticDirectIrradiance = IntegrateDiagnosticHDRIIrradiance(normal, referenceSampleCount, false);

        if(!all(isfinite(diagnosticDirectIrradiance)))
        {
            return float4(diagnosticDirectIrradiance, 1.0f);
        }

        if(lightingDebugView == 24u || lightingDebugView == 25u)
        {
            float3 residual;
            [branch]
            if(lightingDebugView == 25u)
            {
                float3 rotatedIrradiance = IntegrateDiagnosticHDRIIrradiance(normal, 4096u, true);
                residual = diagnosticDirectIrradiance - rotatedIrradiance;
            }
            else
            {
                float3 cubeIrradiance = diffuseIrradiance.SampleLevel(sampler0, normal, 0.0f).rgb;
                residual = cubeIrradiance - diagnosticDirectIrradiance;
            }
            if(!all(isfinite(residual)))
            {
                return float4(1.0f, 0.0f, 1.0f, 1.0f);
            }

            const float residualScale = 10.0f;
            float positive = saturate(max(residual.r, max(residual.g, residual.b)) / residualScale);
            float negative = saturate(-min(residual.r, min(residual.g, residual.b)) / residualScale);
            float3 displayResidual = 0.5f * (1.0f - max(positive, negative)) +
                float3(positive, 0.0f, negative) / (1.0f + min(positive, negative));
            return float4(displayResidual, 1.0f);
        }
    }

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    if(gpuMaterial.model == 0 || gpuMaterial.model == 1)
    {
        if(enableRTGI != 0 && lightingDebugView != 21u && lightingDebugView != 22u && lightingDebugView != 23u && lightingDebugView != 26u && !sourceMipDebug)
        {
            Texture2D<float4> giIrradiance = ResourceDescriptorHeap[giIrradianceIndex];
            irradiance = giIrradiance.Load(int3(pixel, 0)).rgb;
            // Opt-in contact correction after GI denoising; indirect diffuse only.
            if(rtgiContactAOStrength > 0.0f)
            {
                Texture2D<float> aoVisibility = ResourceDescriptorHeap[aoVisibilityIndex];
                float filteredAO = aoVisibility.Load(int3(pixel, 0));

                float aoContrast = pow(saturate(filteredAO), 2.0f);

                irradiance *= lerp(
                    1.0f,
                    aoContrast,
                    saturate(rtgiContactAOStrength)
                );
            }
        }
        else
        {
            irradiance = diffuseIrradiance.SampleLevel(sampler0, normal, 0.0f).rgb;
            float visibility = 1.0f;
            if(lightingDebugView != 22u && lightingDebugView != 23u && lightingDebugView != 26u && !sourceMipDebug)
            {
                Texture2D<float> aoVisibility = ResourceDescriptorHeap[aoVisibilityIndex];
                visibility = saturate(aoVisibility.Load(int3(pixel, 0)));
            }
            irradiance *= visibility;

            if(lightingDebugView == 23u || lightingDebugView == 26u || sourceMipDebug)
            {
                irradiance = diagnosticDirectIrradiance;
            }
        }
    }

    Texture2D<float> specularAOTexture =
        ResourceDescriptorHeap[aoVisibilityIndex];

    float specularVisibility =
        specularAOTexture.Load(int3(pixel, 0));

    float specularAO = lerp(
        1.0f,
        isfinite(specularVisibility) ? saturate(specularVisibility) : 1.0f,
        saturate(rtgiContactAOStrength)
    );

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
            lerp(environmentSpecular * specularAO, rayTracedSpecular, rtWeight);

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
            lerp(environmentSpecular * specularAO, rayTracedSpecular, rtWeight);

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
            lerp(environmentSpecular * specularAO, rayTracedSpecular, rtWeight);
    }

    // Apply receiver material AO once after indirect composition for every model.
    // Keep raw/reference lighting diagnostics independent of material AO.
    if(!lightingDebug)
    {
        environmentDiffuse *= materialAO;
        finalSpecular *= materialAO;
    }

    float3 directional =
        directBRDF *
        lightColor *
        lightIntensity *
        NdotL *
        shadowVisibility;

    // LIGHT.1 table Lights: unshadowed direct light only. They are not RTGI inputs, so nothing
    // here is also counted as indirect light, and material AO stays indirect-only as for the sun.
    float3 sceneLightDirect = float3(0.0f, 0.0f, 0.0f);
    uint boundedLightCount = min(sceneLightCount, SCENE_LIGHT_CAPACITY);

    [loop]
    for(uint lightIndex = 0u; lightIndex < boundedLightCount; ++lightIndex)
    {
        GPULight sceneLight = sceneLights[lightIndex];
        float3 directionToSceneLight;
        float3 incidentRadiance = EvaluateSceneLightIncidence(sceneLight, worldPosition.xyz, directionToSceneLight);
        float sceneLightNdotL = saturate(dot(normal, directionToSceneLight));

        if(sceneLightNdotL <= 0.0f || all(incidentRadiance <= 0.0f))
        {
            continue;
        }

        float3 sceneLightBRDF = EvaluateMaterialDirectBRDF(gpuMaterial, baseColor, normal, directionToSceneLight,
            viewDirection, roughness, NdotV, brdfIntegrationLUT, ggxAverageAlbedoLUT);
        sceneLightDirect += sceneLightBRDF * incidentRadiance * sceneLightNdotL;
    }

    directional += sceneLightDirect;

    float3 lighting =
        directional +
        environmentDiffuse +
        finalSpecular;

    // Observe the exact production terms without changing their calculations.

    if(lightingDebugView == 19u)
    {
        lighting = environmentSpecular;
    }
    else if(lightingDebugView == 20u)
    {
        lighting = directional;
    }
    else if(lightingDebugView == 21u || lightingDebugView == 22u || lightingDebugView == 23u || lightingDebugView == 26u || sourceMipDebug)
    {
        lighting = environmentDiffuse;
    }

    return float4(
        lighting,
        1.0f
    );
}
