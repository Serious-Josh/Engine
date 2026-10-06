#include "MaterialLighting.hlsli"

// Raw metadata for the actual MIS-selected stochastic ray, not a reconstructed
// or in-lobe specular tracking distance. Geometry hits store RayTCurrent() > 0.
// Finite, exactly representable R32_FLOAT sentinels distinguish the other states.
static const float RAW_REFLECTION_RAY_MISS = -1.0f;
static const float RAW_REFLECTION_RAY_INVALID = -2.0f;

struct ReflectionPayload
{
    // Seven 32-bit scalars: 28-byte radiance surface payload.
    float hitDistance;
    float3 worldNormal;
    float2 uv;
    uint materialIndex;
};

struct TrackingPayload
{
    float hitDistance;
};

struct RayTracingInstanceData
{
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint materialIndex;
    uint vertexStride;

    uint normalOffset;
    uint uvOffset;
    uint indexed;
    uint padding;
};

struct ReflectionConstants
{
    row_major float4x4 inverseViewProjection;

    float3 cameraPosition;
    uint depthIndex;

    uint materialIndex;
    uint surfaceIndex;
    uint instanceDataIndex;
    uint materialBufferIndex;

    uint environmentIndex;
    uint environmentCubemapIndex;
    uint diffuseIrradianceIndex;
    uint ggxMultiscatterIrradianceIndex;

    uint brdfIntegrationLUTIndex;
    uint ggxAverageAlbedoLUTIndex;
    uint frameIndex;
    uint importanceConditionalCDFIndex;

    uint importanceMarginalCDFIndex;
    uint importanceWidth;
    uint importanceHeight;
    float importanceTotalWeight;
};

struct DirectionalLightConstants
{
    float3 lightDirection;
    float lightIntensity;

    float3 lightColor;
    float ambientStrength;
};

RaytracingAccelerationStructure scene :
    register(t0);

RWTexture2D<float4> reflectionTexture :
    register(u0);

RWTexture2D<float> hitDistanceTexture :
    register(u1);

RWTexture2D<float> specularTrackingDistanceTexture :
    register(u2);

ConstantBuffer<ReflectionConstants> reflectionConstants :
    register(b0);

ConstantBuffer<DirectionalLightConstants> lightConstants :
    register(b1);

SamplerState materialSampler :
    register(s0);

SamplerState environmentSampler :
    register(s1);

float3 LoadVertexNormal(
    ByteAddressBuffer vertexBuffer,
    uint vertexIndex,
    uint vertexStride,
    uint normalOffset)
{
    uint address =
        vertexIndex * vertexStride +
        normalOffset;

    uint3 packedNormal =
        vertexBuffer.Load3(address);

    return asfloat(packedNormal);
}

float2 LoadVertexUV(
    ByteAddressBuffer vertexBuffer,
    uint vertexIndex,
    uint vertexStride,
    uint uvOffset)
{
    uint address =
        vertexIndex * vertexStride +
        uvOffset;

    uint2 packedUV =
        vertexBuffer.Load2(address);

    return asfloat(packedUV);
}

void GetTriangleVertexIndices(
    RayTracingInstanceData instanceData,
    uint primitiveIndex,
    out uint vertexIndex0,
    out uint vertexIndex1,
    out uint vertexIndex2)
{
    if(instanceData.indexed != 0)
    {
        ByteAddressBuffer indexBuffer =
            ResourceDescriptorHeap[
                NonUniformResourceIndex(
                    instanceData.indexBufferIndex
                )
            ];

        uint indexAddress =
            primitiveIndex *
            3 *
            sizeof(uint);

        vertexIndex0 =
            indexBuffer.Load(
                indexAddress + 0
            );

        vertexIndex1 =
            indexBuffer.Load(
                indexAddress + 4
            );

        vertexIndex2 =
            indexBuffer.Load(
                indexAddress + 8
            );
    }
    else
    {
        vertexIndex0 =
            primitiveIndex * 3 + 0;

        vertexIndex1 =
            primitiveIndex * 3 + 1;

        vertexIndex2 =
            primitiveIndex * 3 + 2;
    }
}

float Random01(uint2 pixel, uint seed)
{
    uint value =
        pixel.x * 1973u +
        pixel.y * 9277u +
        seed * 26699u +
        911u;

    value =
        (value << 13u) ^
        value;

    uint hashed =
        value *
        (
            value *
            value *
            15731u +
            789221u
        ) +
        1376312589u;

    return
        float(hashed & 0x00FFFFFFu) /
        float(0x01000000u);
}

// A/B switch: false preserves the original five Random01 calls.
static const bool USE_SOBOL_REFLECTION_SAMPLING = true;
// First seven 32-bit Joe-Kuo D(6) direction vectors, validated against
// SciPy 1.16.2 scipy.stats.qmc.Sobol(d=7, scramble=False, bits=32).
static const uint REFLECTION_SOBOL_DIRECTIONS[7][32] =
{
    {
        0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u,
        0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u,
        0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u,
        0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u
    },
    {
        0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u, 0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
        0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u, 0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
        0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u, 0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
        0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u, 0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu
    },
    {
        0x80000000u, 0xC0000000u, 0x60000000u, 0x90000000u, 0xE8000000u, 0x5C000000u, 0x8E000000u, 0xC5000000u,
        0x68800000u, 0x9CC00000u, 0xEE600000u, 0x55900000u, 0x80680000u, 0xC09C0000u, 0x60EE0000u, 0x90550000u,
        0xE8808000u, 0x5CC0C000u, 0x8E606000u, 0xC5909000u, 0x6868E800u, 0x9C9C5C00u, 0xEEEE8E00u, 0x5555C500u,
        0x8000E880u, 0xC0005CC0u, 0x60008E60u, 0x9000C590u, 0xE8006868u, 0x5C009C9Cu, 0x8E00EEEEu, 0xC5005555u
    },
    {
        0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u, 0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
        0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u, 0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
        0x208F8000u, 0x51474000u, 0xFBEA2000u, 0x75D93000u, 0xA0858800u, 0x914E5400u, 0xDBE79E00u, 0x25DB6D00u,
        0x58800080u, 0xE54000C0u, 0x79E00020u, 0xB6D00050u, 0x800800F8u, 0xC00C0074u, 0x200200A2u, 0x50050093u
    },
    {
        0x80000000u, 0x40000000u, 0x20000000u, 0xB0000000u, 0xF8000000u, 0xDC000000u, 0x7A000000u, 0x9D000000u,
        0x5A800000u, 0x2FC00000u, 0xA1600000u, 0xF0B00000u, 0xDA880000u, 0x6FC40000u, 0x81620000u, 0x40BB0000u,
        0x22878000u, 0xB3C9C000u, 0xFB65A000u, 0xDDB2D000u, 0x78022800u, 0x9C0B3C00u, 0x5A0FB600u, 0x2D0DDB00u,
        0xA2878080u, 0xF3C9C040u, 0xDB65A020u, 0x6DB2D0B0u, 0x800228F8u, 0x400B3CDCu, 0x200FB67Au, 0xB00DDB9Du
    },
    {
        0x80000000u, 0x40000000u, 0x60000000u, 0x30000000u, 0xC8000000u, 0x24000000u, 0x56000000u, 0xFB000000u,
        0xE0800000u, 0x70400000u, 0xA8600000u, 0x14300000u, 0x9EC80000u, 0xDF240000u, 0xB6D60000u, 0x8BBB0000u,
        0x48008000u, 0x64004000u, 0x36006000u, 0xCB003000u, 0x2880C800u, 0x54402400u, 0xFE605600u, 0xEF30FB00u,
        0x7E48E080u, 0xAF647040u, 0x1EB6A860u, 0x9F8B1430u, 0xD6C81EC8u, 0xBB249F24u, 0x80D6D6D6u, 0x40BBBBBBu
    },
    {
        0x80000000u, 0xC0000000u, 0xA0000000u, 0xD0000000u, 0x58000000u, 0x94000000u, 0x3E000000u, 0xE3000000u,
        0xBE800000u, 0x23C00000u, 0x1E200000u, 0xF3100000u, 0x46780000u, 0x67840000u, 0x78460000u, 0x84670000u,
        0xC6788000u, 0xA784C000u, 0xD846A000u, 0x5467D000u, 0x9E78D800u, 0x33845400u, 0xE6469E00u, 0xB7673300u,
        0x20F86680u, 0x104477C0u, 0xF8668020u, 0x4477C010u, 0x668020F8u, 0x77C01044u, 0x8020F866u, 0xC0104477u
    }
};

uint ReflectionScrambleHash(uint value)
{
    // Bijective 32-bit MurmurHash3 finalizer; integer arithmetic wraps.
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    value ^= value >> 16u;
    return value;
}

uint ReflectionScrambleSeed(uint2 pixel, uint dimension)
{
    // Stable across frames, with distinct dimension seeds for each pixel.
    uint pixelSeed =
        ReflectionScrambleHash(pixel.x ^ 0xA511E9B3u) ^
        ReflectionScrambleHash(pixel.y ^ 0x63D83595u);
    return ReflectionScrambleHash(
        pixelSeed ^ ((dimension + 1u) * 0x9E3779B9u) ^ 0xB5297A4Du
    );
}

uint ReflectionSobolBits(uint sampleIndex, uint dimension)
{
    uint grayIndex = sampleIndex ^ (sampleIndex >> 1u);
    uint coordinate = 0u;
    [unroll]
    for(uint bit = 0u; bit < 32u; ++bit)
    {
        if((grayIndex & (1u << bit)) != 0u)
            coordinate ^= REFLECTION_SOBOL_DIRECTIONS[dimension][bit];
    }
    return coordinate;
}

uint ReflectionNestedOwen24(uint coordinate, uint seed)
{
    // Base-2 nested uniform scrambling: each input-prefix tree node
    // chooses identity or bit-swap. The leading sentinel encodes depth.
    // Process all 24 output digits, including trailing input zeros.
    uint prefix = 1u;
    uint scrambled = 0u;
    [loop]
    for(uint bit = 0u; bit < 24u; ++bit)
    {
        uint inputBit = (coordinate >> (31u - bit)) & 1u;
        uint swapBit = ReflectionScrambleHash(seed ^ prefix) >> 31u;
        scrambled = (scrambled << 1u) | (inputBit ^ swapBit);
        prefix = (prefix << 1u) | inputBit;
    }
    return scrambled;
}

float ReflectionSobol01(uint2 pixel, uint sampleIndex, uint dimension)
{
    uint coordinate = ReflectionSobolBits(sampleIndex, dimension);
    uint scrambled = ReflectionNestedOwen24(
        coordinate, ReflectionScrambleSeed(pixel, dimension)
    );
    // Exactly representable 24-bit integer conversion; never rounds to 1.
    return float(scrambled) * (1.0f / 16777216.0f);
}

uint SampleCDF(
    StructuredBuffer<float> cdf,
    uint offset,
    uint count,
    float randomValue)
{
    uint low =
        0;

    uint high =
        count - 1;

    while(low < high)
    {
        uint middle =
            low +
            (high - low) / 2;

        float cdfValue =
            cdf[
                offset +
                middle
            ];

        if(randomValue < cdfValue)
        {
            high =
                middle;
        }
        else
        {
            low =
                middle + 1;
        }
    }

    return low;
}

uint2 SampleEnvironmentTexel(
    float2 randomSample)
{
    StructuredBuffer<float> conditionalCDF =
        ResourceDescriptorHeap[
            reflectionConstants.importanceConditionalCDFIndex
        ];

    StructuredBuffer<float> marginalCDF =
        ResourceDescriptorHeap[
            reflectionConstants.importanceMarginalCDFIndex
        ];

    uint y =
        SampleCDF(
            marginalCDF,
            0,
            reflectionConstants.importanceHeight,
            randomSample.y
        );

    uint rowOffset =
        y *
        reflectionConstants.importanceWidth;

    uint x =
        SampleCDF(
            conditionalCDF,
            rowOffset,
            reflectionConstants.importanceWidth,
            randomSample.x
        );

    return
        uint2(
            x,
            y
        );
}

float EvaluateEnvironmentPDF(
    float3 direction)
{
    float2 environmentUV =
        DirectionToEquirectangularUV(
            direction
        );

    environmentUV.x =
        frac(environmentUV.x);

    environmentUV.y =
        clamp(
            environmentUV.y,
            0.0f,
            1.0f - 1.0e-7f
        );

    uint x =
        min(
            (uint)(environmentUV.x * reflectionConstants.importanceWidth),
            reflectionConstants.importanceWidth - 1u
        );

    uint y =
        min(
            (uint)(environmentUV.y * reflectionConstants.importanceHeight),
            reflectionConstants.importanceHeight - 1u
        );

    StructuredBuffer<float> conditionalCDF =
        ResourceDescriptorHeap[
            reflectionConstants.importanceConditionalCDFIndex
        ];

    StructuredBuffer<float> marginalCDF =
        ResourceDescriptorHeap[
            reflectionConstants.importanceMarginalCDFIndex
        ];

    float marginalPrevious =
        y > 0u
            ? marginalCDF[y - 1u]
            : 0.0f;

    float rowProbability =
        max(
            0.0f,
            marginalCDF[y] - marginalPrevious
        );

    uint rowOffset =
        y * reflectionConstants.importanceWidth;

    float conditionalPrevious =
        x > 0u
            ? conditionalCDF[rowOffset + x - 1u]
            : 0.0f;

    float columnProbability =
        max(
            0.0f,
            conditionalCDF[rowOffset + x] - conditionalPrevious
        );

    float texelProbability =
        rowProbability *
        columnProbability;

    float sinTheta =
        sin(
            PI * environmentUV.y
        );

    float uvPDF =
        texelProbability *
        float(
            reflectionConstants.importanceWidth *
            reflectionConstants.importanceHeight
        );

    return
        uvPDF /
        max(
            2.0f * PI * PI * sinTheta,
            1.0e-8f
        );
}

float EvaluateGGXVNDFPDF(
    float3 normal,
    float3 viewDirection,
    float3 reflectionDirection,
    float roughness)
{
    float NdotV =
        dot(
            normal,
            viewDirection
        );

    float NdotL =
        dot(
            normal,
            reflectionDirection
        );

    if(NdotV <= 0.0f || NdotL <= 0.0f)
    {
        return 0.0f;
    }

    float3 halfVectorSum =
        viewDirection +
        reflectionDirection;

    float halfVectorLengthSquared =
        dot(
            halfVectorSum,
            halfVectorSum
        );

    if(!isfinite(halfVectorLengthSquared) ||
        halfVectorLengthSquared <= 1.0e-8f)
    {
        return 0.0f;
    }

    float3 halfVector =
        halfVectorSum *
        rsqrt(halfVectorLengthSquared);

    float VdotH =
        dot(
            viewDirection,
            halfVector
        );

    if(VdotH <= 0.0f)
    {
        return 0.0f;
    }

    float D =
        DistributionGGX(
            normal,
            halfVector,
            roughness
        );

    float alpha =
        max(
            roughness,
            0.045f
        );

    alpha *= alpha;

    float alphaSquared =
        alpha * alpha;

    return
        D /
        (
            2.0f *
            (
                NdotV +
                sqrt(
                    alphaSquared +
                    (1.0f - alphaSquared) *
                    NdotV * NdotV
                )
            )
        );
}

float3 SampleGGXReflectionDirection(
    float3 normal,
    float3 viewDirection,
    float roughness,
    float2 randomSample,
    out float3 halfVector)
{
    if(roughness <= 0.001f)
    {
        halfVector = normal;

        return
            reflect(
                -viewDirection,
                normal
            );
    }

    float alpha =
        max(
            roughness,
            0.045f
        );

    alpha *= alpha;

    float3 tangent =
        normalize(
            abs(normal.z) < 0.999f
                ? cross(
                    float3(0.0f, 0.0f, 1.0f),
                    normal
                )
                : cross(
                    float3(0.0f, 1.0f, 0.0f),
                    normal
                )
        );

    float3 bitangent =
        cross(
            normal,
            tangent
        );

    float3 localView =
        float3(
            dot(viewDirection, tangent),
            dot(viewDirection, bitangent),
            dot(viewDirection, normal)
        );

    float3 stretchedView =
        normalize(
            float3(
                alpha * localView.x,
                alpha * localView.y,
                localView.z
            )
        );

    float lensq =
        stretchedView.x * stretchedView.x +
        stretchedView.y * stretchedView.y;

    float3 T1 =
        lensq > 0.0f
            ? float3(
                -stretchedView.y,
                stretchedView.x,
                0.0f
            ) * rsqrt(lensq)
            : float3(
                1.0f,
                0.0f,
                0.0f
            );

    float3 T2 =
        cross(
            stretchedView,
            T1
        );

    float radius =
        sqrt(randomSample.y);

    float phi =
        2.0f * PI * randomSample.x;

    float t1 =
        radius * cos(phi);

    float t2 =
        radius * sin(phi);

    float s =
        0.5f *
        (1.0f + stretchedView.z);

    t2 =
        (1.0f - s) *
        sqrt(
            max(
                0.0f,
                1.0f - t1 * t1
            )
        ) +
        s * t2;

    float3 hemisphereNormal =
        t1 * T1 +
        t2 * T2 +
        sqrt(
            max(
                0.0f,
                1.0f -
                t1 * t1 -
                t2 * t2
            )
        ) *
        stretchedView;

    float3 localHalfVector =
        normalize(
            float3(
                alpha * hemisphereNormal.x,
                alpha * hemisphereNormal.y,
                max(
                    0.0f,
                    hemisphereNormal.z
                )
            )
        );

    halfVector =
        normalize(
            tangent * localHalfVector.x +
            bitangent * localHalfVector.y +
            normal * localHalfVector.z
        );

    return
        normalize(
            reflect(
                -viewDirection,
                halfVector
            )
        );
}

float3 SampleReflectionEnvironment(float3 rayDirection)
{
    Texture2D<float4> environmentTexture =
        ResourceDescriptorHeap[
            reflectionConstants.environmentIndex
        ];

    float3 worldDirection =
        normalize(
            rayDirection
        );

    float2 environmentUV =
        DirectionToEquirectangularUV(
            worldDirection
        );

    return
        environmentTexture.SampleLevel(
            environmentSampler,
            environmentUV,
            0.0f
        ).rgb;
}

// Components are exposed for later continuation; their existing sum is unchanged.
struct SecondaryLighting
{
    float3 directional;
    float3 environmentDiffuse;
    float3 singleScatterEnvironmentSpecular;
    float3 multipleScatterEnvironmentSpecular;
};

// Called only from RayGen: independent local state, never NRD tracking metadata.
float HardDirectionalVisibility(float3 worldPosition, float3 worldNormal)
{
    if(!all(isfinite(worldPosition)) || !all(isfinite(worldNormal)) ||
        !all(isfinite(lightConstants.lightDirection)))
        return 0.0f;

    float3 normal = normalize(worldNormal);
    float3 directionToLight = normalize(-lightConstants.lightDirection);
    if(!all(isfinite(normal)) || !all(isfinite(directionToLight)) ||
        !(dot(normal, directionToLight) > 0.0f))
        return 0.0f;

    RayDesc visibilityRay;
    visibilityRay.Origin = worldPosition + normal * 0.001f;
    visibilityRay.Direction = directionToLight;
    visibilityRay.TMin = 0.001f;
    visibilityRay.TMax = 10000.0f;
    TrackingPayload visibilityPayload;
    visibilityPayload.hitDistance = RAW_REFLECTION_RAY_INVALID;
    TraceRay(scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF, 1, 1, 1, visibilityRay, visibilityPayload);
    return visibilityPayload.hitDistance == RAW_REFLECTION_RAY_MISS ? 1.0f : 0.0f;
}

float3 ShadeSecondarySurface(ReflectionPayload surface, float3 rayDirection, float directionalVisibility,
    out SecondaryLighting lighting)
{
    lighting = (SecondaryLighting)0;
    StructuredBuffer<GPUMaterial> materialBuffer =
        ResourceDescriptorHeap[reflectionConstants.materialBufferIndex];
    GPUMaterial material = materialBuffer[surface.materialIndex];
    float3 worldNormal = surface.worldNormal;
    float2 hitUV = surface.uv;
    Texture2D<float4> materialTexture =
        ResourceDescriptorHeap[
            NonUniformResourceIndex(
                material.textureIndex
            )
        ];

    float3 baseColor =
        material.baseColor *
        materialTexture.SampleLevel(
            materialSampler,
            hitUV,
            0.0f
        ).rgb;

    float3 directionToLight =
        -lightConstants.lightDirection;

    float3 viewDirection =
        normalize(
            -rayDirection
        );

    float NdotL =
        saturate(
            dot(
                worldNormal,
                directionToLight
            )
        );

    float NdotV =
        saturate(
            dot(
                worldNormal,
                viewDirection
            )
        );

    TextureCube<float4> environmentCubemap =
        ResourceDescriptorHeap[
            reflectionConstants.environmentCubemapIndex
        ];

    TextureCube<float4> diffuseIrradiance =
        ResourceDescriptorHeap[
            reflectionConstants.diffuseIrradianceIndex
        ];

    TextureCube<float4> ggxMultiscatterIrradiance =
        ResourceDescriptorHeap[
            reflectionConstants.ggxMultiscatterIrradianceIndex
        ];

    Texture2D<float2> brdfIntegrationLUT =
        ResourceDescriptorHeap[
            reflectionConstants.brdfIntegrationLUTIndex
        ];

    Texture2D<float> ggxAverageAlbedoLUT =
        ResourceDescriptorHeap[
            reflectionConstants.ggxAverageAlbedoLUTIndex
        ];

    float directionalAlbedoLight =
        0.0f;

    float directionalAlbedoView =
        0.0f;

    float averageAlbedo =
        0.0f;

    if(NdotL > 0.0f && NdotV > 0.0f)
    {
        float2 directionalAlbedoLightData =
            brdfIntegrationLUT.SampleLevel(
                materialSampler,
                float2(
                    NdotL,
                    material.roughness
                ),
                0.0f
            ).rg;

        float2 directionalAlbedoViewData =
            brdfIntegrationLUT.SampleLevel(
                materialSampler,
                float2(
                    NdotV,
                    material.roughness
                ),
                0.0f
            ).rg;

        directionalAlbedoLight =
            directionalAlbedoLightData.x +
            directionalAlbedoLightData.y;

        directionalAlbedoView =
            directionalAlbedoViewData.x +
            directionalAlbedoViewData.y;

        averageAlbedo =
            ggxAverageAlbedoLUT.SampleLevel(
                materialSampler,
                float2(
                    material.roughness,
                    0.5f
                ),
                0.0f
            ).r;
    }

    float3 directBRDF =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    if(material.model == 0)
    {
        directBRDF =
            EvaluateMetallicRoughnessDirectBRDF(
                material,
                baseColor,
                worldNormal,
                directionToLight,
                viewDirection,
                directionalAlbedoLight,
                directionalAlbedoView,
                averageAlbedo
            );
    }
    else if(material.model == 1)
    {
        directBRDF =
            EvaluateDielectricDirectBRDF(
                material,
                baseColor,
                worldNormal,
                directionToLight,
                viewDirection,
                directionalAlbedoLight,
                directionalAlbedoView,
                averageAlbedo
            );
    }
    else if(material.model == 2)
    {
        directBRDF =
            EvaluateConductorDirectBRDF(
                material,
                worldNormal,
                directionToLight,
                viewDirection,
                directionalAlbedoLight,
                directionalAlbedoView,
                averageAlbedo
            );
    }

    float3 directional =
        directBRDF *
        lightConstants.lightColor *
        lightConstants.lightIntensity *
        NdotL *
        directionalVisibility;

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

    float3 reflectionDirection =
        reflect(
            -viewDirection,
            worldNormal
        );

    float environmentMip =
        material.roughness *
        10.0f;

    float3 prefilteredEnvironment =
        environmentCubemap.SampleLevel(
            materialSampler,
            reflectionDirection,
            environmentMip
        ).rgb;

    float2 integratedBRDF =
        brdfIntegrationLUT.SampleLevel(
            materialSampler,
            float2(
                NdotV,
                material.roughness
            ),
            0.0f
        ).rg;

    float directionalEnvironmentAlbedo =
        saturate(
            integratedBRDF.x +
            integratedBRDF.y
        );

    float environmentAverageAlbedo =
        ggxAverageAlbedoLUT.SampleLevel(
            materialSampler,
            float2(
                material.roughness,
                0.5f
            ),
            0.0f
        ).r;

    float multiscatterMip =
        material.roughness *
        6.0f;

    float3 multiscatterIrradiance =
        ggxMultiscatterIrradiance.SampleLevel(
            materialSampler,
            worldNormal,
            multiscatterMip
        ).rgb;

    if(material.model == 0)
    {
        float metallic =
            saturate(
                material.metallic
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

        environmentSpecular =
            prefilteredEnvironment *
            (
                F0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float3 averageFresnel =
            AverageSchlickFresnel(
                F0
            );

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                environmentAverageAlbedo,
                averageFresnel
            );

        float multiscatterNormalization =
            max(
                1.0f -
                environmentAverageAlbedo,
                1.0e-6f
            );

        lighting.singleScatterEnvironmentSpecular = environmentSpecular;
        lighting.multipleScatterEnvironmentSpecular =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalEnvironmentAlbedo) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular += lighting.multipleScatterEnvironmentSpecular;

        float3 irradiance =
            diffuseIrradiance.SampleLevel(
                materialSampler,
                worldNormal,
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
    else if(material.model == 1)
    {
        float iorDifference =
            material.ior - 1.0f;

        float iorSum =
            material.ior + 1.0f;

        float dielectricF0 =
            (iorDifference * iorDifference) /
            max(
                iorSum * iorSum,
                1.0e-6f
            );

        float3 F0 =
            float3(
                dielectricF0,
                dielectricF0,
                dielectricF0
            );

        environmentSpecular =
            prefilteredEnvironment *
            (
                F0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float3 averageFresnel =
            AverageSchlickFresnel(
                F0
            );

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                environmentAverageAlbedo,
                averageFresnel
            );

        float multiscatterNormalization =
            max(
                1.0f -
                environmentAverageAlbedo,
                1.0e-6f
            );

        lighting.singleScatterEnvironmentSpecular = environmentSpecular;
        lighting.multipleScatterEnvironmentSpecular =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalEnvironmentAlbedo) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular += lighting.multipleScatterEnvironmentSpecular;

        float3 irradiance =
            diffuseIrradiance.SampleLevel(
                materialSampler,
                worldNormal,
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
    else if(material.model == 2)
    {
        float3 conductorF0 =
            FresnelConductor(
                1.0f,
                material.conductorN,
                material.conductorK
            );

        environmentSpecular =
            prefilteredEnvironment *
            (
                conductorF0 *
                integratedBRDF.x +
                integratedBRDF.y
            );

        float3 multipleScatterFresnel =
            EvaluateGGXMultipleScatterFresnel(
                environmentAverageAlbedo,
                material.averageConductorFresnel
            );

        float multiscatterNormalization =
            max(
                1.0f -
                environmentAverageAlbedo,
                1.0e-6f
            );

        lighting.singleScatterEnvironmentSpecular = environmentSpecular;
        lighting.multipleScatterEnvironmentSpecular =
            multiscatterIrradiance *
            multipleScatterFresnel *
            (1.0f - directionalEnvironmentAlbedo) *
            INV_PI /
            multiscatterNormalization;

        environmentSpecular += lighting.multipleScatterEnvironmentSpecular;
    }

    lighting.directional = directional;
    lighting.environmentDiffuse = environmentDiffuse;
    return directional + environmentDiffuse + environmentSpecular;
}

// Deterministic delta-mirror weight, matching the existing primary mirror model handling.
float3 EvaluateSecondaryMirrorFresnel(GPUMaterial material, float2 uv, float cosine)
{
    if(material.model == 0)
    {
        Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.textureIndex)];
        float3 baseColor = material.baseColor * texture.SampleLevel(materialSampler, uv, 0.0f).rgb;
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, saturate(material.metallic));
        return FresnelSchlick(cosine, F0);
    }
    if(material.model == 1)
    {
        float difference = material.ior - 1.0f;
        float sum = material.ior + 1.0f;
        float F0 = (difference * difference) / max(sum * sum, 1.0e-6f);
        return FresnelSchlick(cosine, float3(F0, F0, F0));
    }
    if(material.model == 2)
        return FresnelConductor(cosine, material.conductorN, material.conductorK);
    return float3(0.0f, 0.0f, 0.0f);
}

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pixelIndex =
        DispatchRaysIndex().xy;

    uint2 dimensions =
        DispatchRaysDimensions().xy;

    Texture2D<float> depthTexture =
        ResourceDescriptorHeap[
            reflectionConstants.depthIndex
        ];

    Texture2D<float4> surfaceTexture =
        ResourceDescriptorHeap[
            reflectionConstants.surfaceIndex
        ];

    Texture2D<float4> materialTexture =
        ResourceDescriptorHeap[
            reflectionConstants.materialIndex
        ];

    float depth =
        depthTexture.Load(
            int3(
                pixelIndex,
                0
            )
        );

    if(depth >= 1.0f)
    {
        reflectionTexture[pixelIndex] =
            float4(
                0.0f,
                0.0f,
                0.0f,
                1.0f
            );

        hitDistanceTexture[pixelIndex] =
            RAW_REFLECTION_RAY_INVALID;

        specularTrackingDistanceTexture[pixelIndex] =
            RAW_REFLECTION_RAY_INVALID;

        return;
    }

    float2 uv =
        (float2(pixelIndex) + 0.5f) /
        float2(dimensions);

    float2 ndc;

    ndc.x =
        uv.x * 2.0f - 1.0f;

    ndc.y =
        1.0f - uv.y * 2.0f;

    float4 clipPosition =
        float4(
            ndc,
            depth,
            1.0f
        );

    float4 worldPosition =
        mul(
            reflectionConstants.inverseViewProjection,
            clipPosition
        );

    worldPosition /=
        worldPosition.w;

    float4 surfaceData =
        surfaceTexture.Load(
            int3(
                pixelIndex,
                0
            )
        );

    float4 materialData =
        materialTexture.Load(
            int3(
                pixelIndex,
                0
            )
        );

    float3 primaryBaseColor =
        materialData.rgb;

    float3 normal =
        normalize(
            surfaceData.rgb
        );

    uint materialIndex =
        (uint)round(
            surfaceData.a
        );

    StructuredBuffer<GPUMaterial> materialBuffer =
        ResourceDescriptorHeap[
            reflectionConstants.materialBufferIndex
        ];

    GPUMaterial primaryMaterial =
        materialBuffer[
            materialIndex
        ];

    float3 viewDirection =
        normalize(
            reflectionConstants.cameraPosition -
            worldPosition.xyz
        );

    uint sampleIndex = reflectionConstants.frameIndex;
    float strategySample;
    float2 primarySample;
    float2 environmentJitter;

    if(USE_SOBOL_REFLECTION_SAMPLING)
    {
        // Fixed dimensions: strategy; phi/X-CDF; radius/Y-CDF; U/V jitter.
        strategySample = ReflectionSobol01(pixelIndex, sampleIndex, 0u);
        primarySample = float2(
            ReflectionSobol01(pixelIndex, sampleIndex, 1u),
            ReflectionSobol01(pixelIndex, sampleIndex, 2u)
        );
        environmentJitter = float2(
            ReflectionSobol01(pixelIndex, sampleIndex, 3u),
            ReflectionSobol01(pixelIndex, sampleIndex, 4u)
        );
    }
    else
    {
        uint randomSeedBase =
            sampleIndex * 5u;

        strategySample =
            Random01(
                pixelIndex,
                randomSeedBase
            );

        primarySample =
            float2(
                Random01(
                    pixelIndex,
                    randomSeedBase + 1u
                ),
                Random01(
                    pixelIndex,
                    randomSeedBase + 2u
                )
            );

        environmentJitter =
            float2(
                Random01(
                    pixelIndex,
                    randomSeedBase + 3u
                ),
                Random01(
                    pixelIndex,
                    randomSeedBase + 4u
                )
            );
    }

    static const float GGX_STRATEGY_PROBABILITY = 0.5f;

    bool mirrorReflection =
        primaryMaterial.roughness <= 0.001f;

    bool useGGXStrategy =
        mirrorReflection ||
        strategySample < GGX_STRATEGY_PROBABILITY;

    float3 halfVector =
        normal;

    float3 reflectionDirection;

    if(useGGXStrategy)
    {
        reflectionDirection =
            SampleGGXReflectionDirection(
                normal,
                viewDirection,
                primaryMaterial.roughness,
                primarySample,
                halfVector
            );
    }
    else
    {
        uint2 environmentTexel =
            SampleEnvironmentTexel(
                primarySample
            );

        float2 environmentUV =
            (
                float2(environmentTexel) +
                environmentJitter
            ) /
            float2(
                reflectionConstants.importanceWidth,
                reflectionConstants.importanceHeight
            );

        reflectionDirection =
            EquirectangularUVToDirection(
                environmentUV
            );

    }

    bool validReflectionSample =
        dot(normal, viewDirection) > 0.0f &&
        dot(reflectionDirection, normal) > 0.0f;

    if(!mirrorReflection)
    {
        // Use the same half-vector validity criterion as the VNDF PDF.
        float3 halfVectorSum =
            viewDirection + reflectionDirection;

        float halfVectorLengthSquared =
            dot(halfVectorSum, halfVectorSum);

        if(isfinite(halfVectorLengthSquared) &&
            halfVectorLengthSquared > 1.0e-8f)
        {
            halfVector =
                halfVectorSum * rsqrt(halfVectorLengthSquared);

            validReflectionSample =
                validReflectionSample &&
                dot(viewDirection, halfVector) > 0.0f;
        }
        else
        {
            validReflectionSample = false;
        }
    }

    RayDesc ray;

    ray.Origin =
        worldPosition.xyz +
        normal * 0.001f;

    ray.Direction =
        reflectionDirection;

    ray.TMin =
        0.001f;

    ray.TMax =
        10000.0f;

    ReflectionPayload payload;

    payload.worldNormal = float3(0.0f, 0.0f, 0.0f);
    payload.uv = float2(0.0f, 0.0f);
    payload.materialIndex = 0u;
    float3 secondaryRadiance = float3(0.0f, 0.0f, 0.0f);

    payload.hitDistance =
        RAW_REFLECTION_RAY_INVALID;

    if(validReflectionSample)
    {
        TraceRay(
            scene,
            RAY_FLAG_NONE,
            0xFF,
            0,
            1,
            0,
            ray,
            payload
        );



    }

    float trackingHitDistance = RAW_REFLECTION_RAY_INVALID;

    if(useGGXStrategy && validReflectionSample)
    {
        trackingHitDistance = payload.hitDistance;
    }
    else
    {
        // Auxiliary guide samples are independent of the radiance Sobol sequence.
        uint trackingSeed = ReflectionScrambleHash(
            ReflectionScrambleHash(pixelIndex.x ^ 0xD1B54A35u) ^
            ReflectionScrambleHash(pixelIndex.y ^ 0x94D049BBu) ^
            ReflectionScrambleHash(reflectionConstants.frameIndex ^ 0x369DEA0Fu));
        float2 trackingSample = float2(
            ReflectionScrambleHash(trackingSeed ^ 0xA24BAED5u) >> 8u,
            ReflectionScrambleHash(trackingSeed ^ 0x9FB21C65u) >> 8u
        ) * (1.0f / 16777216.0f);

        float3 trackingHalfVector;
        float3 trackingDirection = SampleGGXReflectionDirection(
            normal, viewDirection, primaryMaterial.roughness,
            trackingSample, trackingHalfVector);
        bool validTrackingSample =
            dot(normal, viewDirection) > 0.0f &&
            dot(trackingDirection, normal) > 0.0f;

        if(!mirrorReflection)
        {
            float3 halfVectorSum = viewDirection + trackingDirection;
            float halfVectorLengthSquared = dot(halfVectorSum, halfVectorSum);
            if(isfinite(halfVectorLengthSquared) && halfVectorLengthSquared > 1.0e-8f)
            {
                trackingHalfVector = halfVectorSum * rsqrt(halfVectorLengthSquared);
                validTrackingSample = validTrackingSample &&
                    dot(viewDirection, trackingHalfVector) > 0.0f;
            }
            else
            {
                validTrackingSample = false;
            }
        }

        if(validTrackingSample)
        {
            RayDesc trackingRay = ray;
            trackingRay.Direction = trackingDirection;
            TrackingPayload trackingPayload;
            trackingPayload.hitDistance = RAW_REFLECTION_RAY_INVALID;
            TraceRay(scene, RAY_FLAG_NONE, 0xFF, 1, 1, 1,
                trackingRay, trackingPayload);
            trackingHitDistance = trackingPayload.hitDistance;
        }
    }

    specularTrackingDistanceTexture[pixelIndex] = trackingHitDistance;

    if(payload.hitDistance > 0.0f)
    {
        float3 secondaryPosition = ray.Origin + ray.Direction * payload.hitDistance;
        float secondaryVisibility = HardDirectionalVisibility(secondaryPosition, payload.worldNormal);
        SecondaryLighting secondaryLighting;
        secondaryRadiance = ShadeSecondarySurface(payload, ray.Direction, secondaryVisibility, secondaryLighting);

        GPUMaterial secondaryMaterial = materialBuffer[payload.materialIndex];
        // Preserve the validated deterministic mirror continuation.
        if(secondaryMaterial.roughness <= 0.001f && secondaryMaterial.model <= 2u)
        {
            float3 secondaryView = normalize(-ray.Direction);
            float secondaryNdotV = dot(payload.worldNormal, secondaryView);
            float3 bounce2Direction = normalize(reflect(ray.Direction, payload.worldNormal));
            if(secondaryNdotV > 0.0f && dot(payload.worldNormal, bounce2Direction) > 0.0f &&
                all(isfinite(bounce2Direction)) && all(isfinite(secondaryPosition)))
            {
                RayDesc bounce2Ray;
                bounce2Ray.Origin = secondaryPosition + payload.worldNormal * 0.001f;
                bounce2Ray.Direction = bounce2Direction;
                bounce2Ray.TMin = 0.001f;
                bounce2Ray.TMax = 10000.0f;
                // Keep the first payload intact for raw metadata and NRD tracking.
                ReflectionPayload bounce2Payload = (ReflectionPayload)0;
                bounce2Payload.hitDistance = RAW_REFLECTION_RAY_INVALID;
                TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounce2Ray, bounce2Payload);

                float3 tertiaryRadiance = float3(0.0f, 0.0f, 0.0f);
                if(bounce2Payload.hitDistance > 0.0f)
                {
                    float3 tertiaryPosition = bounce2Ray.Origin + bounce2Ray.Direction * bounce2Payload.hitDistance;
                    float tertiaryVisibility = HardDirectionalVisibility(tertiaryPosition, bounce2Payload.worldNormal);
                    // Terminal shading: even a tertiary mirror cannot launch bounce 3.
                    SecondaryLighting tertiaryLighting;
                    tertiaryRadiance = ShadeSecondarySurface(bounce2Payload, bounce2Ray.Direction, tertiaryVisibility, tertiaryLighting);
                }
                else if(bounce2Payload.hitDistance == RAW_REFLECTION_RAY_MISS)
                {
                    tertiaryRadiance = SampleReflectionEnvironment(bounce2Ray.Direction);
                }
                float3 secondaryFresnel = EvaluateSecondaryMirrorFresnel(
                    secondaryMaterial, payload.uv, saturate(secondaryNdotV));
                // Replace, never add to, the old single-scatter environment approximation.
                secondaryRadiance = secondaryLighting.directional + secondaryLighting.environmentDiffuse +
                    (secondaryFresnel * tertiaryRadiance + secondaryLighting.multipleScatterEnvironmentSpecular);
            }
        }
        else if(secondaryMaterial.roughness > 0.001f && secondaryMaterial.roughness < 0.30f && secondaryMaterial.model <= 2u)
        {
            float3 sampledSecondarySpecular = float3(0.0f, 0.0f, 0.0f);
            float3 secondaryView = normalize(-ray.Direction);
            float2 secondarySample = float2(
                ReflectionSobol01(pixelIndex, sampleIndex, 5u),
                ReflectionSobol01(pixelIndex, sampleIndex, 6u));
            float3 secondaryHalfVector;
            float3 bounce2Direction = SampleGGXReflectionDirection(
                payload.worldNormal, secondaryView, secondaryMaterial.roughness,
                secondarySample, secondaryHalfVector);
            float secondaryNdotV = saturate(dot(payload.worldNormal, secondaryView));
            float secondaryNdotL = saturate(dot(payload.worldNormal, bounce2Direction));
            float secondaryVdotH = saturate(dot(secondaryView, secondaryHalfVector));

            if(secondaryNdotV > 0.0f && secondaryNdotL > 0.0f && secondaryVdotH > 0.0f &&
                isfinite(secondaryMaterial.roughness) && all(isfinite(secondaryView)) &&
                all(isfinite(secondaryHalfVector)) && all(isfinite(bounce2Direction)) &&
                all(isfinite(secondaryPosition)))
            {
                float secondaryPDF = EvaluateGGXVNDFPDF(
                    payload.worldNormal, secondaryView, bounce2Direction, secondaryMaterial.roughness);
                if(isfinite(secondaryPDF) && secondaryPDF > 0.0f)
                {
                    // The material Fresnel helper accepts a cosine; rough GGX uses V dot H.
                    float3 F2 = EvaluateSecondaryMirrorFresnel(
                        secondaryMaterial, payload.uv, secondaryVdotH);
                    float D2 = DistributionGGX(
                        payload.worldNormal, secondaryHalfVector, secondaryMaterial.roughness);
                    float G2 = GeometrySmithGGX(
                        secondaryNdotL, secondaryNdotV, secondaryMaterial.roughness);
                    float3 W2 = F2 * D2 * G2 / (4.0f * secondaryNdotV * secondaryPDF);
                    if(all(isfinite(W2)))
                    {
                        RayDesc bounce2Ray;
                        bounce2Ray.Origin = secondaryPosition + payload.worldNormal * 0.001f;
                        bounce2Ray.Direction = bounce2Direction;
                        bounce2Ray.TMin = 0.001f;
                        bounce2Ray.TMax = 10000.0f;
                        // Separate terminal payload leaves first-segment metadata untouched.
                        ReflectionPayload bounce2Payload = (ReflectionPayload)0;
                        bounce2Payload.hitDistance = RAW_REFLECTION_RAY_INVALID;
                        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounce2Ray, bounce2Payload);

                        float3 tertiaryRadiance = float3(0.0f, 0.0f, 0.0f);
                        if(bounce2Payload.hitDistance > 0.0f)
                        {
                            float3 tertiaryPosition = bounce2Ray.Origin + bounce2Ray.Direction * bounce2Payload.hitDistance;
                            float tertiaryVisibility = HardDirectionalVisibility(tertiaryPosition, bounce2Payload.worldNormal);
                            SecondaryLighting tertiaryLighting;
                            tertiaryRadiance = ShadeSecondarySurface(
                                bounce2Payload, bounce2Ray.Direction, tertiaryVisibility, tertiaryLighting);
                        }
                        else if(bounce2Payload.hitDistance == RAW_REFLECTION_RAY_MISS)
                        {
                            tertiaryRadiance = SampleReflectionEnvironment(bounce2Ray.Direction);
                        }
                        sampledSecondarySpecular = W2 * tertiaryRadiance;
                    }
                }
            }
            // Rejections stay zero in the RT term; only roughness controls the IBL blend.
            float transition = smoothstep(0.25f, 0.30f, secondaryMaterial.roughness);
            float3 secondarySingleScatter = lerp(
                sampledSecondarySpecular, secondaryLighting.singleScatterEnvironmentSpecular, transition);
            secondaryRadiance = secondaryLighting.directional + secondaryLighting.environmentDiffuse +
                secondarySingleScatter + secondaryLighting.multipleScatterEnvironmentSpecular;
        }
    }
    else if(payload.hitDistance == RAW_REFLECTION_RAY_MISS)
    {
        secondaryRadiance = SampleReflectionEnvironment(ray.Direction);
    }

    if(validReflectionSample)
    {
        float NdotV =
            saturate(
                dot(
                    normal,
                    viewDirection
                )
            );

        float NdotL =
            saturate(
                dot(
                    normal,
                    reflectionDirection
                )
            );

        float VdotH =
            saturate(
                dot(
                    viewDirection,
                    halfVector
                )
            );

        float3 F =
            float3(
                0.0f,
                0.0f,
                0.0f
            );

        if(primaryMaterial.model == 0)
        {
            float3 dielectricF0 =
                float3(
                    0.04f,
                    0.04f,
                    0.04f
                );

            float3 F0 =
                lerp(
                    dielectricF0,
                    primaryBaseColor,
                    saturate(
                        primaryMaterial.metallic
                    )
                );

            F =
                FresnelSchlick(
                    VdotH,
                    F0
                );
        }
        else if(primaryMaterial.model == 1)
        {
            float iorDifference =
                primaryMaterial.ior - 1.0f;

            float iorSum =
                primaryMaterial.ior + 1.0f;

            float dielectricF0 =
                (iorDifference * iorDifference) /
                max(
                    iorSum * iorSum,
                    1.0e-6f
                );

            F =
                FresnelSchlick(
                    VdotH,
                    float3(
                        dielectricF0,
                        dielectricF0,
                        dielectricF0
                    )
                );
        }
        else if(primaryMaterial.model == 2)
        {
            F =
                FresnelConductor(
                    VdotH,
                    primaryMaterial.conductorN,
                    primaryMaterial.conductorK
                );
        }

        float3 sampleWeight =
            float3(
                0.0f,
                0.0f,
                0.0f
            );

        if(mirrorReflection)
        {
            sampleWeight = F;
        }
        else if(NdotV > 0.0f && NdotL > 0.0f && VdotH > 0.0f)
        {
            float D =
                DistributionGGX(
                    normal,
                    halfVector,
                    primaryMaterial.roughness
                );

            float G =
                GeometrySmithGGX(
                    NdotL,
                    NdotV,
                    primaryMaterial.roughness
                );

            float ggxPDF =
                EvaluateGGXVNDFPDF(
                    normal,
                    viewDirection,
                    reflectionDirection,
                    primaryMaterial.roughness
                );

            float environmentPDF =
                EvaluateEnvironmentPDF(
                    reflectionDirection
                );

            float environmentStrategyProbability =
                1.0f - GGX_STRATEGY_PROBABILITY;

            float mixturePDF =
                GGX_STRATEGY_PROBABILITY *
                ggxPDF +
                environmentStrategyProbability *
                environmentPDF;

            if(mixturePDF > 0.0f)
            {
                sampleWeight =
                    F * D * G /
                    (4.0f * NdotV * mixturePDF);
            }
        }

        secondaryRadiance *=
            sampleWeight;
    }

    hitDistanceTexture[pixelIndex] =
        payload.hitDistance;

    reflectionTexture[pixelIndex] =
        float4(
            secondaryRadiance,
            1.0f
        );
}

[shader("miss")]
void TrackingMiss(inout TrackingPayload payload)
{
    payload.hitDistance = RAW_REFLECTION_RAY_MISS;
}

[shader("closesthit")]
void TrackingClosestHit(
    inout TrackingPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hitDistance = RayTCurrent();
}

[shader("miss")]
void Miss(
    inout ReflectionPayload payload)
{
    payload.hitDistance = RAW_REFLECTION_RAY_MISS;


}

[shader("closesthit")]
void ClosestHit(
    inout ReflectionPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hitDistance =
        RayTCurrent();

    StructuredBuffer<RayTracingInstanceData>
        instanceDataBuffer =
            ResourceDescriptorHeap[
                reflectionConstants.instanceDataIndex
            ];

    RayTracingInstanceData instanceData =
        instanceDataBuffer[
            InstanceID()
        ];

    ByteAddressBuffer vertexBuffer =
        ResourceDescriptorHeap[
            NonUniformResourceIndex(
                instanceData.vertexBufferIndex
            )
        ];

    uint vertexIndex0;
    uint vertexIndex1;
    uint vertexIndex2;

    GetTriangleVertexIndices(
        instanceData,
        PrimitiveIndex(),
        vertexIndex0,
        vertexIndex1,
        vertexIndex2
    );

    float barycentric0 =
        1.0f -
        attributes.barycentrics.x -
        attributes.barycentrics.y;

    float barycentric1 =
        attributes.barycentrics.x;

    float barycentric2 =
        attributes.barycentrics.y;

    float2 uv0 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex0,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 uv1 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex1,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 uv2 =
        LoadVertexUV(
            vertexBuffer,
            vertexIndex2,
            instanceData.vertexStride,
            instanceData.uvOffset
        );

    float2 hitUV =
        uv0 * barycentric0 +
        uv1 * barycentric1 +
        uv2 * barycentric2;

    float3 normal0 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex0,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 normal1 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex1,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 normal2 =
        LoadVertexNormal(
            vertexBuffer,
            vertexIndex2,
            instanceData.vertexStride,
            instanceData.normalOffset
        );

    float3 objectNormal =
        normalize(
            normal0 * barycentric0 +
            normal1 * barycentric1 +
            normal2 * barycentric2
        );

    float3x3 objectToWorld =
        (float3x3)ObjectToWorld3x4();

    float3 worldNormal =
        normalize(
            mul(
                objectToWorld,
                objectNormal
            )
        );

    payload.worldNormal = worldNormal;
    payload.uv = hitUV;
    payload.materialIndex = instanceData.materialIndex;
}
