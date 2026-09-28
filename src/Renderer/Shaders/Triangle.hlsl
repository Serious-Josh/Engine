struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};

cbuffer MaterialConstants : register(b0)
{
    uint textureIndex;
}

SamplerState sampler0 : register(s0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    output.position = float4(input.position, 1.0f);
    output.uv = input.uv;

    return output;
}

PSOutput PSMain(VSOutput input)
{
    Texture2D<float4> texture0 = ResourceDescriptorHeap[textureIndex];

    PSOutput output;

    output.albedo = texture0.Sample(sampler0, input.uv);

    // Temporary constant normal.
    // Later this will come from the actual geometry.
    output.normal = float4(0.0f, 0.0f, 1.0f, 1.0f);

    return output;
}