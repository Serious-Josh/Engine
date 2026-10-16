// Sixteen root constants; CPU Matrix4 and HLSL both use row-major storage, column vectors.
cbuffer ColliderConstants : register(b0)
{
    row_major float4x4 clipFromBox;
};

float3 BoxEdgeEndpoint(uint vertexIndex)
{
    // Four parallel edges per axis, each with two endpoints. The remaining axes
    // choose the four fixed sides. This ordering matches ColliderBoxVertex on CPU.
    const uint endpointsPerEdge = 2;
    const uint edgesPerAxis = 4;
    const uint axisCount = 3;
    uint edgeIndex = vertexIndex / endpointsPerEdge;
    uint edgeAxis = edgeIndex / edgesPerAxis;
    uint sideIndex = edgeIndex % edgesPerAxis;
    uint firstSideAxis = (edgeAxis + 1) % axisCount;
    uint secondSideAxis = (edgeAxis + 2) % axisCount;

    float3 localPosition = 0.0;
    localPosition[edgeAxis] = float(vertexIndex % endpointsPerEdge) - 0.5;
    localPosition[firstSideAxis] = float(sideIndex % 2) - 0.5;
    localPosition[secondSideAxis] = float(sideIndex / 2) - 0.5;
    return localPosition;
}

float4 VSMain(uint vertexIndex : SV_VertexID) : SV_POSITION
{
    float3 localPosition = BoxEdgeEndpoint(vertexIndex);
    return mul(clipFromBox, float4(localPosition, 1));
}

float4 PSMain() : SV_TARGET
{
    return float4(0.1, 1.0, 0.35, 1.0);
}
