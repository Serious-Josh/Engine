// AIR.4E.2 Editor divergence points: the temperature points' placement (EditorAirTemperature.hlsl, AirVisualization.hlsli's anchor
// placement), one round point per fluid piece of the cell holding each block's world-stable point, coloured by that piece's divergence
// from CSDivergence's buffer (AirDivergence.hlsl). Every piece is drawn, so divergence-free air shows as neutral points; an absent piece,
// or a cell in solid, draws nothing. Reads the published field and the divergence buffer only.
//
// Colour: t = (outflow / cellSize) / displayRange (1/s), clamped to [-1, 1]; neutral grey at 0, blending to orange toward +1 (net
// outflow: a source, air expanding) and to cyan toward -1 (net inflow: a sink, air compressing). Drawn after tone mapping.

#include "AirVisualization.hlsli"

cbuffer EditorAirDivergenceConstants : register(b0)
{
    row_major float4x4 viewProjection;
    AirFieldBinding field;
    AirVectorParameters parameters;
    // CSDivergence's output, read-only; the divergence (1/s) at which the colour saturates; the point's half size in normalized device
    // coordinates per axis.
    uint divergenceIndex;
    float displayRange;
    float2 pointHalfSize;
    // AirDivergenceMeasure: 0 and 2 show the face value (the operator's, or with a projection its predicted value after it or its value
    // before it), 1 the collocated value (the stored velocity's).
    uint measure;
    uint3 divergencePadding;
};

struct DivergenceVertex
{
    float4 position : SV_Position;
    nointerpolation float3 color : COLOR0;
    float2 corner : TEXCOORD0;
};

static const float3 NeutralColor = float3(0.82, 0.82, 0.82);
static const float3 SourceColor = float3(1.0, 0.5, 0.0);
static const float3 SinkColor = float3(0.0, 0.75, 0.85);

float3 DivergenceColor(float divergence)
{
    const float t = clamp(divergence / max(displayRange, 1e-6), -1.0, 1.0);
    return lerp(NeutralColor, t >= 0.0 ? SourceColor : SinkColor, abs(t));
}

DivergenceVertex VSMain(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
{
    // Clockwise on screen, the front face of every Engine pipeline (back faces are culled).
    static const float2 Corners[6] = {float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, 1.0),
        float2(1.0, -1.0)};
    const uint blockIndex = instance / parameters.vectorsPerBlock;
    const int3 block = parameters.firstBlock + int3(blockIndex % parameters.blocks.x, (blockIndex / parameters.blocks.x) % parameters.blocks.y,
        blockIndex / (parameters.blocks.x * parameters.blocks.y));
    const AirPathField path = AirVectorPath(field, parameters);
    const AirVectorSource source = AirAnchorSource(path, parameters, block, instance % parameters.vectorsPerBlock);
    const float2 corner = Corners[vertexIndex];

    DivergenceVertex output;
    output.corner = corner;
    output.color = NeutralColor;

    if(field.valid == 0 || !source.valid)
    {
        // In front of the near plane: clipped entirely.
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        return output;
    }

    ByteAddressBuffer divergence = ResourceDescriptorHeap[NonUniformResourceIndex(divergenceIndex)];
    const float4 measures = asfloat(divergence.Load4(AirVelocityAddress(field, AirSlotOf(field, source.cell))));
    const float2 pieceMeasures = source.piece == 0 ? measures.xy : measures.zw;
    const float outflow = measure == 1 ? pieceMeasures.y : pieceMeasures.x;

    const float3 world = (float3(field.minimum) + source.position) * field.cellSize;
    const float4 centre = mul(viewProjection, float4(world, 1.0));
    output.position = float4(centre.xy + corner * pointHalfSize * centre.w, centre.zw);
    output.color = DivergenceColor(outflow * field.inverseCellSize);
    return output;
}

float4 PSMain(DivergenceVertex input) : SV_Target
{
    if(dot(input.corner, input.corner) > 1.0)
    {
        discard;
    }

    return float4(input.color, 1.0);
}
