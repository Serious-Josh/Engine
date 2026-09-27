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

Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    output.position =
        float4(input.position, 1.0f);

    output.uv = input.uv;

    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return texture0.Sample(
        sampler0,
        input.uv
    );
}