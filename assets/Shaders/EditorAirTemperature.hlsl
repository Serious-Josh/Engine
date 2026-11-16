// AIR.4F F4.5.1b.1 Editor temperature points: one camera-facing square (6 vertices, drawn as a round point) per fluid piece of the
// cell holding each block's world-stable point (AirVisualization.hlsli's anchor placement), at that piece's anchor, coloured by the
// piece's ΔT (velocity.w: kelvin from the scene's ambient). Every piece the sample cells hold is drawn, whatever its temperature or
// velocity, so ambient air shows as neutral points; an absent piece, or a cell in solid, draws nothing. A point keeps a fixed size on
// screen. Reads the published air field only: nothing is read back to the CPU, and the simulation is unchanged.
//
// Colour: t = ΔT / displayRange, clamped to [-1, 1]; neutral grey at 0, blending to blue toward -1 and to red toward +1. Drawn after
// tone mapping, so exposure and lighting never change it.

#include "AirVisualization.hlsli"

cbuffer EditorAirTemperatureConstants : register(b0)
{
    row_major float4x4 viewProjection;
    AirFieldBinding field;
    AirVectorParameters parameters;
    // Kelvin at which the colour saturates, and the point's half size in normalized device coordinates per axis.
    float displayRange;
    float2 pointHalfSize;
    float temperaturePadding;
};

struct TemperatureVertex
{
    float4 position : SV_Position;
    nointerpolation float3 color : COLOR0;
    float2 corner : TEXCOORD0;
};

static const float3 NeutralColor = float3(0.82, 0.82, 0.82);
static const float3 HotColor = float3(1.0, 0.12, 0.06);
static const float3 ColdColor = float3(0.1, 0.35, 1.0);

float3 TemperatureColor(float temperature)
{
    const float t = clamp(temperature / max(displayRange, 1e-3), -1.0, 1.0);
    return lerp(NeutralColor, t >= 0.0 ? HotColor : ColdColor, abs(t));
}

TemperatureVertex VSMain(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
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

    TemperatureVertex output;
    output.corner = corner;
    output.color = NeutralColor;

    if(field.valid == 0 || !source.valid)
    {
        // In front of the near plane: clipped entirely.
        output.position = float4(0.0, 0.0, -1.0, 1.0);
        return output;
    }

    const float3 world = (float3(field.minimum) + source.position) * field.cellSize;
    const float4 centre = mul(viewProjection, float4(world, 1.0));
    output.position = float4(centre.xy + corner * pointHalfSize * centre.w, centre.zw);
    output.color = TemperatureColor(AirPieceState(path, source.cell, source.piece).w);
    return output;
}

float4 PSMain(TemperatureVertex input) : SV_Target
{
    if(dot(input.corner, input.corner) > 1.0)
    {
        discard;
    }

    return float4(input.color, 1.0);
}
