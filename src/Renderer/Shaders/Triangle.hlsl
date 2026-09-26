struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    if (vertexID == 0)
        output.position = float4(0.0f, 0.5f, 0.0f, 1.0f);
    else if (vertexID == 1)
        output.position = float4(0.5f, -0.5f, 0.0f, 1.0f);
    else
        output.position = float4(-0.5f, -0.5f, 0.0f, 1.0f);

    return output;
}

float4 PSMain() : SV_TARGET
{
    return float4(1.0f, 0.2f, 0.1f, 1.0f);
}