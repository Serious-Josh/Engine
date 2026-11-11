// AIR.4D D3: what EditorAir.hlsl (the lean pass and the split-cell classification) and EditorAirSplit.hlsl (the split-cell pass)
// share: the constants (EditorAirPass.cpp, EditorAirConstants), the vertex output and the pixel shader.
//
// A vector is drawn by exactly one of the two passes. The lean pass draws every instance whose sample is plain (all eight corners
// whole cells or outside the window) and hides the rest; CSClassify lists the rest; the split pass draws only those, with the full
// D2 sampling. Keeping the split-cell sampling out of the lean shader keeps its register use, and so its cost, that of a plain
// trilinear sample (the solver's two passes are split the same way).

#ifndef EDITOR_AIR_VECTORS_HLSLI
#define EDITOR_AIR_VECTORS_HLSLI

#include "AirVisualization.hlsli"

cbuffer EditorAirConstants : register(b0)
{
    row_major float4x4 viewProjection;
    AirFieldBinding field;
    AirVectorParameters parameters;
    // The split pass's instance list (one instance index per entry) and its draw arguments (D3D12_DRAW_ARGUMENTS: the instance count
    // is the list's length), writable views.
    uint splitListIndex;
    uint splitArgumentsIndex;
    uint2 constantsPadding;
};

struct VectorVertex
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

// A vector not drawn is placed in front of the near plane (clipped).
VectorVertex VectorVertexOf(AirVector vector, uint vertexIndex)
{
    VectorVertex output;
    const float3 position = vertexIndex == 0 ? vector.start : vector.end;
    output.position = vector.visible ? mul(viewProjection, float4(position, 1.0)) : float4(0.0, 0.0, -1.0, 1.0);
    // The tail is dimmer than the head, so direction reads at a glance.
    output.color = float4(vector.color * (vertexIndex == 0 ? 0.35 : 1.0), 1.0);
    return output;
}

float4 PSMain(VectorVertex input) : SV_Target
{
    return input.color;
}

#endif
