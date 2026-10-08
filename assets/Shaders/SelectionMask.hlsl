struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

cbuffer CameraConstants : register(b1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    row_major float4x4 view;
    row_major float4x4 previousView;
};

cbuffer ObjectConstants : register(b2)
{
    row_major float4x4 model;
    row_major float4x4 previousModel;
    row_major float4x4 normalMatrix;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;

    float4 worldPosition = mul(model, float4(input.position, 1.0f));
    output.position = mul(viewProjection, worldPosition);

    return output;
}

float PSMain(VSOutput input) : SV_Target0
{
    return 1.0f;
}