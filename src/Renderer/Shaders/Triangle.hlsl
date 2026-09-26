struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    output.position = float4(input.position, 1.0f);

    return output;
}

float4 PSMain() : SV_TARGET
{
    return float4(1.0f, 0.2f, 0.1f, 1.0f);
}