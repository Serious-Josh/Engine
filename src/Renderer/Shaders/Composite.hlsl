struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer GBufferConstants : register(b0)
{
    uint albedoIndex;
    uint normalIndex;
    uint depthIndex;
}

cbuffer ReconstructionConstants : register(b1)
{
    row_major float4x4 inverseViewProjection;
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

    output.position = float4(positions[vertexID], 0.0f, 1.0f);
    output.uv = uvs[vertexID];

    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[albedoIndex];
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];

    float depth = depthTexture.Sample(sampler0, input.uv);

    if(depth >= 1.0f){
        return float4(0.1f, 0.2f, 0.4f, 1.0f);
    }

    float2 ndc;
    ndc.x = input.uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - input.uv.y * 2.0f;

    float4 clipPosition = float4(ndc, depth, 1.0f);

    float4 worldPosition = mul(inverseViewProjection, clipPosition);
    worldPosition /= worldPosition.w;

    float3 displayPosition = abs(worldPosition.xyz);

    return float4(displayPosition, 1.0f);
}