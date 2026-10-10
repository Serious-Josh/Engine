struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer MipConstants : register(b0)
{
    uint textureIndex;
    uint sourceMip;
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
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    output.position =
        float4(positions[vertexID], 0.0f, 1.0f);

    output.uv =
        uvs[vertexID];

    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    Texture2D<float4> sourceTexture =
        ResourceDescriptorHeap[textureIndex];

    return sourceTexture.SampleLevel(
        sampler0,
        input.uv,
        float(sourceMip)
    );
}
