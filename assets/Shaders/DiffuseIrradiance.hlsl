#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer IrradianceConstants : register(b0)
{
    uint sourceTextureIndex;
    uint faceIndex;
}

SamplerState linearSampler : register(s0);

float RadicalInverseVanDerCorput(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);

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

    return
        float(bits) *
        2.3283064365386963e-10f;
}

float2 Hammersley(
    uint index,
    uint sampleCount)
{
    return float2(
        float(index) / float(sampleCount),
        RadicalInverseVanDerCorput(index)
    );
}

float3 CosineSampleHemisphere(float2 xi)
{
    float radius =
        sqrt(xi.x);

    float phi =
        2.0f * PI * xi.y;

    float x =
        radius * cos(phi);

    float y =
        radius * sin(phi);

    float z =
        sqrt(
            max(
                0.0f,
                1.0f - xi.x
            )
        );

    return float3(
        x,
        y,
        z
    );
}

float3 CubemapDirection(
    uint face,
    float2 uv)
{
    float2 coordinates =
        uv * 2.0f - 1.0f;

    coordinates.y =
        -coordinates.y;

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

void BuildBasis(
    float3 normal,
    out float3 tangent,
    out float3 bitangent)
{
    float3 up =
        abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f)
        : float3(1.0f, 0.0f, 0.0f);

    tangent =
        normalize(
            cross(
                up,
                normal
            )
        );

    bitangent =
        cross(
            normal,
            tangent
        );
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

    float sampleSolidAngle =
        1.0f /
        max(
            float(sampleCount) * pdf,
            0.000001f
        );

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

    float sourceMip =
        0.5f *
        log2(
            sampleSolidAngle /
            max(
                texelSolidAngle,
                1.0e-12f
            )
        );

    return clamp(
        sourceMip,
        0.0f,
        float(sourceMipCount - 1)
    );
}

VSOutput VSMain(
    uint vertexID : SV_VertexID)
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

float4 PSMain(
    VSOutput input) : SV_Target
{
    Texture2D<float4> sourceTexture =
        ResourceDescriptorHeap[
            sourceTextureIndex
        ];

    float3 normal =
        CubemapDirection(
            faceIndex,
            input.uv
        );

    float3 tangent;
    float3 bitangent;

    BuildBasis(
        normal,
        tangent,
        bitangent
    );

    static const uint SAMPLE_COUNT =
        4096u;

    float3 irradiance =
        float3(
            0.0f,
            0.0f,
            0.0f
        );

    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 xi =
            Hammersley(
                i,
                SAMPLE_COUNT
            );

        float3 localDirection =
            CosineSampleHemisphere(xi);

        float3 sampleDirection =
            normalize(
                tangent *
                localDirection.x +
                bitangent *
                localDirection.y +
                normal *
                localDirection.z
            );

        float NdotL =
            saturate(
                dot(
                    normal,
                    sampleDirection
                )
            );

        float pdf =
            NdotL / PI;

        float sourceMip =
            CalculateSourceMip(
                sourceTexture,
                sampleDirection,
                pdf,
                SAMPLE_COUNT
            );

        float2 environmentUV =
            DirectionToEquirectangularUV(
                sampleDirection
            );

        float3 sampleColor =
            sourceTexture.SampleLevel(
                linearSampler,
                environmentUV,
                sourceMip
            ).rgb;

        irradiance +=
            sampleColor;
    }

    irradiance *=
        PI /
        float(SAMPLE_COUNT);

    return float4(
        irradiance,
        1.0f
    );
}