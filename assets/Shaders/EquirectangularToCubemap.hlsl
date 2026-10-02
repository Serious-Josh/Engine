#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer CubemapConstants : register(b0)
{
    uint sourceTextureIndex;
    uint faceIndex;
    uint mipIndex;
    uint mipCount;
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
        float4(positions[vertexID], 0.0f, 1.0f);

    output.uv =
        uvs[vertexID];

    return output;
}

float RadicalInverseVanDerCorput(uint bits)
{
    bits =
        (bits << 16u) |
        (bits >> 16u);

    bits =
        ((bits & 0x55555555u) << 1u) |
        ((bits & 0xAAAAAAAAu) >> 1u);

    bits =
        ((bits & 0x33333333u) << 2u) |
        ((bits & 0xCCCCCCCCu) >> 2u);

    bits =
        ((bits & 0x0F0F0F0Fu) << 4u) |
        ((bits & 0xF0F0F0F0u) >> 4u);

    bits =
        ((bits & 0x00FF00FFu) << 8u) |
        ((bits & 0xFF00FF00u) >> 8u);

    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint sampleCount)
{
    return float2(
        float(index) / float(sampleCount),
        RadicalInverseVanDerCorput(index)
    );
}

float DistributionGGX(
    float NdotH,
    float roughness)
{
    float alpha =
        roughness * roughness;

    float alphaSquared =
        alpha * alpha;

    float denominator =
        NdotH * NdotH *
        (alphaSquared - 1.0f) +
        1.0f;

    return
        alphaSquared /
        max(
            PI * denominator * denominator,
            0.000001f
        );
}

float3 ImportanceSampleGGX(
    float2 xi,
    float3 normal,
    float roughness)
{
    float alpha =
        roughness * roughness;

    float alphaSquared =
        alpha * alpha;

    float phi =
        2.0f * PI * xi.x;

    float cosTheta =
        sqrt(
            (1.0f - xi.y) /
            (
                1.0f +
                (alphaSquared - 1.0f) * xi.y
            )
        );

    float sinTheta =
        sqrt(
            max(
                0.0f,
                1.0f -
                cosTheta * cosTheta
            )
        );

    float3 halfVectorTangent =
        float3(
            cos(phi) * sinTheta,
            sin(phi) * sinTheta,
            cosTheta
        );

    float3 up =
        abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);

    float3 tangent =
        normalize(
            cross(up, normal)
        );

    float3 bitangent =
        cross(normal, tangent);

    return normalize(
        tangent * halfVectorTangent.x +
        bitangent * halfVectorTangent.y +
        normal * halfVectorTangent.z
    );
}

float3 UniformSampleSphere(float2 xi)
{
    float z =
        1.0f - 2.0f * xi.x;

    float radius =
        sqrt(
            max(
                0.0f,
                1.0f - z * z
            )
        );

    float phi =
        2.0f * PI * xi.y;

    return float3(
        radius * cos(phi),
        z,
        radius * sin(phi)
    );
}

float3 CubemapDirection(uint face, float2 uv)
{
    float2 coordinates =
        uv * 2.0f - 1.0f;

    coordinates.y = -coordinates.y;

    switch(face)
    {
        case 0:
            return normalize(
                float3(
                    1.0f,
                    coordinates.y,
                    -coordinates.x
                )
            );

        case 1:
            return normalize(
                float3(
                    -1.0f,
                    coordinates.y,
                    coordinates.x
                )
            );

        case 2:
            return normalize(
                float3(
                    coordinates.x,
                    1.0f,
                    -coordinates.y
                )
            );

        case 3:
            return normalize(
                float3(
                    coordinates.x,
                    -1.0f,
                    coordinates.y
                )
            );

        case 4:
            return normalize(
                float3(
                    coordinates.x,
                    coordinates.y,
                    1.0f
                )
            );

        default:
            return normalize(
                float3(
                    -coordinates.x,
                    coordinates.y,
                    -1.0f
                )
            );
    }
}

float CalculateSourceMip(
    Texture2D<float4> sourceTexture,
    float3 sampleDirection,
    float pdf,
    uint sampleCount)
{
    uint sourceWidth;
    uint sourceHeight;
    uint sourceMipCount;

    sourceTexture.GetDimensions(
        0,
        sourceWidth,
        sourceHeight,
        sourceMipCount
    );

    // A single GGX sample represents approximately
    // this much solid angle.
    float sampleSolidAngle =
        1.0f /
        max(
            float(sampleCount) * pdf,
            0.000001f
        );

    // Equirectangular texels do not all cover the
    // same solid angle. Their area shrinks toward
    // the poles by cos(latitude).
    float cosLatitude =
        sqrt(
            max(
                1.0f -
                sampleDirection.y *
                sampleDirection.y,
                0.000001f
            )
        );

    float longitudeStep =
        (2.0f * PI) /
        float(sourceWidth);

    float latitudeStep =
        PI /
        float(sourceHeight);

    float texelSolidAngle =
        longitudeStep *
        latitudeStep *
        cosLatitude;

    // Each mip level increases a texel's area by
    // approximately four, hence 0.5 * log2().
    float sourceMip =
        0.5f *
        log2(
            sampleSolidAngle /
            max(
                texelSolidAngle,
                0.000001f
            )
        );

    return clamp(
        sourceMip,
        0.0f,
        float(sourceMipCount - 1)
    );
}

float4 PSMain(VSOutput input) : SV_Target0
{
    Texture2D<float4> sourceTexture =
        ResourceDescriptorHeap[sourceTextureIndex];

    float3 normal =
        CubemapDirection(
            faceIndex,
            input.uv
        );

    float roughness =
        (mipCount > 1)
        ? float(mipIndex) /
          float(mipCount - 1)
        : 0.0f;

    // Preserve mip 0 as the unfiltered environment.
    if(mipIndex == 0)
    {
        float2 environmentUV =
            DirectionToEquirectangularUV(normal);

        return sourceTexture.SampleLevel(
            sampler0,
            environmentUV,
            0.0f
        );
    }

    // At maximum roughness the reflection is extremely broad.
    // A 1x1 cubemap mip cannot remain continuous if each face
    // stores a different value, so all six terminal faces use
    // the same spherical average of the environment.
    if(mipIndex == mipCount - 1)
    {
        static const uint SPHERE_SAMPLE_COUNT = 2048u;

        float3 averageColor =
            float3(0.0f, 0.0f, 0.0f);

        for(uint i = 0u; i < SPHERE_SAMPLE_COUNT; ++i)
        {
            float2 xi =
                Hammersley(
                    i,
                    SPHERE_SAMPLE_COUNT
                );

            float3 sampleDirection =
                UniformSampleSphere(xi);

            float2 environmentUV =
                DirectionToEquirectangularUV(
                    sampleDirection
                );

            averageColor +=
                sourceTexture.SampleLevel(
                    sampler0,
                    environmentUV,
                    0.0f
                ).rgb;
        }

        averageColor /=
            float(SPHERE_SAMPLE_COUNT);

        return float4(
            averageColor,
            1.0f
        );
    }

    // For environment prefiltering we use N = V = R.
    float3 viewDirection =
        normal;

    float3 prefilteredColor =
        float3(0.0f, 0.0f, 0.0f);

    float totalWeight =
        0.0f;

    static const uint SAMPLE_COUNT = 512u;

    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 xi =
            Hammersley(
                i,
                SAMPLE_COUNT
            );

        float3 halfVector =
            ImportanceSampleGGX(
                xi,
                normal,
                roughness
            );

        float HdotV =
            saturate(
                dot(
                    halfVector,
                    viewDirection
                )
            );

        float3 lightDirection =
            normalize(
                2.0f *
                HdotV *
                halfVector -
                viewDirection
            );

        float NdotL =
            saturate(
                dot(
                    normal,
                    lightDirection
                )
            );

        if(NdotL > 0.0f)
        {
            float NdotH =
                saturate(
                    dot(
                        normal,
                        halfVector
                    )
                );

            float distribution =
                DistributionGGX(
                    NdotH,
                    roughness
                );

            // Convert the half-vector GGX PDF into
            // the corresponding reflected-direction PDF.
            float pdf =
                (
                    distribution *
                    NdotH
                ) /
                max(
                    4.0f * HdotV,
                    0.000001f
                );

            float sourceMip =
                CalculateSourceMip(
                    sourceTexture,
                    lightDirection,
                    pdf,
                    SAMPLE_COUNT
                );

            float2 environmentUV =
                DirectionToEquirectangularUV(
                    lightDirection
                );

            float3 sampleColor =
                sourceTexture.SampleLevel(
                    sampler0,
                    environmentUV,
                    sourceMip
                ).rgb;

            prefilteredColor +=
                sampleColor * NdotL;

            totalWeight +=
                NdotL;
        }
    }

    if(totalWeight > 0.0f)
    {
        prefilteredColor /=
            totalWeight;
    }

    return float4(
        prefilteredColor,
        1.0f
    );
}