#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

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

float RadicalInverseVdC(uint bits)
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

    return
        float(bits) *
        2.3283064365386963e-10f;
}

float2 Hammersley(
    uint index,
    uint sampleCount)
{
    return float2(
        float(index) /
        float(sampleCount),
        RadicalInverseVdC(index)
    );
}

float3 ImportanceSampleGGX(
    float2 xi,
    float roughness,
    float3 normal)
{
    float alpha =
        roughness * roughness;

    float alpha2 =
        alpha * alpha;

    float phi =
        2.0f * PI * xi.x;

    float cosTheta =
        sqrt(
            (1.0f - xi.y) /
            (
                1.0f +
                (alpha2 - 1.0f) *
                xi.y
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

    float3 halfVector;

    halfVector.x =
        cos(phi) * sinTheta;

    halfVector.y =
        sin(phi) * sinTheta;

    halfVector.z =
        cosTheta;

    float3 up =
        abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);

    float3 tangent =
        normalize(
            cross(
                up,
                normal
            )
        );

    float3 bitangent =
        cross(
            normal,
            tangent
        );

    return normalize(
        tangent * halfVector.x +
        bitangent * halfVector.y +
        normal * halfVector.z
    );
}

float GeometrySchlickGGXIBL(
    float NdotV,
    float roughness)
{
    float alpha =
        roughness * roughness;

    return
        (2.0f * NdotV) /
        (
            NdotV +
            sqrt(
                alpha * alpha +
                (1.0f - alpha * alpha) *
                NdotV * NdotV
            )
        );
}

float IntegrateDirectionalAlbedo(
    float NdotV,
    float roughness)
{
    float3 normal =
        float3(
            0.0f,
            0.0f,
            1.0f
        );

    float3 viewDirection =
        float3(
            sqrt(
                max(
                    0.0f,
                    1.0f -
                    NdotV * NdotV
                )
            ),
            0.0f,
            NdotV
        );

    float directionalAlbedo =
        0.0f;

    static const uint SAMPLE_COUNT =
        256u;

    for(
        uint i = 0u;
        i < SAMPLE_COUNT;
        ++i)
    {
        float2 xi =
            Hammersley(
                i,
                SAMPLE_COUNT
            );

        float3 halfVector =
            ImportanceSampleGGX(
                xi,
                roughness,
                normal
            );

        float3 lightDirection =
            normalize(
                2.0f *
                dot(
                    viewDirection,
                    halfVector
                ) *
                halfVector -
                viewDirection
            );

        float NdotL =
            saturate(
                lightDirection.z
            );

        if(NdotL > 0.0f)
        {
            float NdotH =
                saturate(
                    halfVector.z
                );

            float VdotH =
                saturate(
                    dot(
                        viewDirection,
                        halfVector
                    )
                );

            float G =
                GeometrySchlickGGXIBL(
                    NdotV,
                    roughness
                ) *
                GeometrySchlickGGXIBL(
                    NdotL,
                    roughness
                );

            float GVis =
                (G * VdotH) /
                max(
                    NdotH * NdotV,
                    0.0001f
                );

            directionalAlbedo +=
                GVis;
        }
    }

    return
        directionalAlbedo /
        float(SAMPLE_COUNT);
}

float IntegrateAverageAlbedo(
    float roughness)
{
    static const uint MU_SAMPLE_COUNT =
        64u;

    float averageAlbedo =
        0.0f;

    for(
        uint i = 0u;
        i < MU_SAMPLE_COUNT;
        ++i)
    {
        // Midpoint integration over mu in [0, 1].
        float mu =
            (
                float(i) +
                0.5f
            ) /
            float(MU_SAMPLE_COUNT);

        float directionalAlbedo =
            IntegrateDirectionalAlbedo(
                mu,
                roughness
            );

        averageAlbedo +=
            directionalAlbedo *
            mu;
    }

    return
        2.0f *
        averageAlbedo /
        float(MU_SAMPLE_COUNT);
}

float PSMain(VSOutput input) : SV_Target0
{
    float roughness =
        saturate(
            input.uv.x
        );

    return
        IntegrateAverageAlbedo(
            roughness
        );
}