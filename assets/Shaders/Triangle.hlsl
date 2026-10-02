struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
};

struct PSOutput
{
    float4 material : SV_Target0;
    float4 surface : SV_Target1;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
    float roughness;
    uint materialIndex;
    float padding;

    float3 baseColor;
    float padding2;
}

cbuffer CameraConstants : register(b1)
{
    row_major float4x4 viewProjection;
}

cbuffer ObjectConstants : register(b2)
{
    row_major float4x4 model;
    row_major float4x4 normalMatrix;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    float4 worldPosition = mul(model, float4(input.position, 1.0f));
    output.position = mul(viewProjection, worldPosition);

    output.worldNormal = normalize(mul((float3x3)normalMatrix, input.normal));
    output.uv = input.uv;

    return output;
}

PSOutput PSMain(VSOutput input)
{
    PSOutput output;

    output.material = float4(
        baseColor,
        roughness
    );

    output.surface = float4(
        normalize(input.worldNormal),
        float(materialIndex)
    );

    return output;
}