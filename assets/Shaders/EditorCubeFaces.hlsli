// Faces of an axis-aligned unit cube for Editor diagnostics drawn as 36 vertices per instance (six faces of two triangles).
// Front faces are clockwise from outside, as for every Engine mesh (EditorVoxel.hlsl uses the same order).

#ifndef EDITOR_CUBE_FACES_HLSLI
#define EDITOR_CUBE_FACES_HLSLI

// The corner (0 or 1 per axis) of face `face` (-X, +X, -Y, +Y, -Z, +Z) for vertex `corner` of its two triangles, with its
// (u, v) across the face: clockwise-front for the positive face, mirrored for the negative one.
float3 CubeFaceCorner(uint face, uint corner, out float2 uv)
{
    const uint axis = face / 2;
    const bool positive = (face % 2) == 1;
    const uint first = (axis + 1) % 3;
    const uint second = (axis + 2) % 3;
    static const float2 positiveOrder[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    static const float2 negativeOrder[6] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    uv = positive ? positiveOrder[corner] : negativeOrder[corner];

    float3 position = 0.0;
    position[axis] = positive ? 1.0 : 0.0;
    position[first] = uv.x;
    position[second] = uv.y;
    return position;
}

// Fixed per-face shading, so faces read without lighting.
float CubeFaceShade(uint face)
{
    static const float shade[6] = {0.70, 0.85, 0.55, 1.0, 0.62, 0.78};
    return shade[face];
}

// A translucent face with an opaque outline about a pixel and a half wide.
float4 OutlinedFace(float3 color, float2 faceCoordinate)
{
    const float2 edge = min(faceCoordinate, 1.0 - faceCoordinate) / max(fwidth(faceCoordinate), 1e-6);
    return float4(color, min(edge.x, edge.y) < 1.5 ? 0.9 : 0.22);
}

#endif
