// VX.4C.2 Editor voxel overlay. Layouts and constants match src/Renderer/EditorVoxelOverlay.h; keep them identical.
//
// Cubes: 36 vertices per instance (six faces of two triangles), generated here; a face whose exposed bit is clear collapses to
// a degenerate triangle. Front faces are clockwise from outside (cross(B - A, C - A) points outward), as for every Engine mesh.
// Boxes: 24 line-list vertices per region (twelve edges).
// Positions are in lattice units (voxels relative to the anchor region's origin); clipFromLattice maps them to clip space.

cbuffer VoxelConstants : register(b0)
{
    row_major float4x4 clipFromLattice;
    uint regionVoxels;
};

struct Cell
{
    uint word0;
    uint word1;
};

struct Region
{
    int3 offset;
    uint state;
};

StructuredBuffer<Cell> cells : register(t0);
StructuredBuffer<Region> regions : register(t1);

static const uint RegionSlotMask = 0xFFFF;
static const uint FaceMaskShift = 16;
static const uint ClassShift = 22;
static const uint ScaleShift = 23;
static const uint LocalBits = 10;
static const uint LocalMask = (1u << LocalBits) - 1;

static const uint CellClassSurface = 0;
static const uint CellScaleVoxel = 0;
static const uint CellScaleBrick = 1;
static const float BrickVoxels = 8.0;

static const uint BoxPopulated = 0;
static const uint BoxUniformInside = 1;
static const uint BoxError = 2;

struct OverlayVertex
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
};

// The corner (0 or 1 per axis) of face `face` (bit order -X, +X, -Y, +Y, -Z, +Z) for vertex `corner` of its two triangles.
float3 FaceCorner(uint face, uint corner)
{
    const uint axis = face / 2;
    const bool positive = (face % 2) == 1;
    const uint first = (axis + 1) % 3;
    const uint second = (axis + 2) % 3;

    // (u, v) across the face, with cross(first, second) = +axis: this order is clockwise-front for the positive face, and the
    // negative face uses the mirrored order.
    static const float2 positiveOrder[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    static const float2 negativeOrder[6] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    const float2 uv = positive ? positiveOrder[corner] : negativeOrder[corner];

    float3 position = 0.0;
    position[axis] = positive ? 1.0 : 0.0;
    position[first] = uv.x;
    position[second] = uv.y;
    return position;
}

float3 CellColor(uint cellClass, uint face)
{
    const float3 surface = float3(1.0, 0.55, 0.15);
    const float3 interior = float3(0.25, 0.55, 1.0);
    // Fixed per-axis shading so individual cubes stay readable without lighting.
    static const float shade[6] = {0.70, 0.85, 0.55, 1.0, 0.62, 0.78};
    return (cellClass == CellClassSurface ? surface : interior) * shade[face];
}

OverlayVertex CellVS(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    const Cell cell = cells[instanceIndex];
    const uint face = vertexIndex / 6;
    const Region region = regions[cell.word0 & RegionSlotMask];
    const bool exposed = ((cell.word0 >> (FaceMaskShift + face)) & 1u) != 0;
    const uint scale = (cell.word0 >> ScaleShift) & 3u;
    const float size = scale == CellScaleVoxel ? 1.0 : (scale == CellScaleBrick ? BrickVoxels : float(regionVoxels));
    const float3 local = float3(cell.word1 & LocalMask, (cell.word1 >> LocalBits) & LocalMask, (cell.word1 >> (2 * LocalBits)) & LocalMask);
    const float3 lattice = float3(region.offset) * float(regionVoxels) + local + FaceCorner(face, vertexIndex % 6) * size;

    OverlayVertex output;
    // A hidden face is placed in front of the near plane (z < 0), so its triangles are clipped entirely.
    output.position = exposed ? mul(clipFromLattice, float4(lattice, 1.0)) : float4(0.0, 0.0, -1.0, 1.0);
    output.color = float4(CellColor((cell.word0 >> ClassShift) & 1u, face), 1.0);
    return output;
}

float3 BoxColor(uint state)
{
    switch(state)
    {
    case BoxPopulated:
        return float3(0.85, 0.85, 0.85);
    case BoxUniformInside:
        return float3(0.3, 0.5, 1.0);
    default:
        return float3(1.0, 0.1, 0.1);
    }
}

// Twelve edges of the unit cube [0, 1]^3, two endpoints each, as in EditorCollider.hlsl's BoxEdgeEndpoint.
float3 BoxEdgeEndpoint(uint vertexIndex)
{
    const uint edgeIndex = vertexIndex / 2;
    const uint edgeAxis = edgeIndex / 4;
    const uint sideIndex = edgeIndex % 4;

    float3 position = 0.0;
    position[edgeAxis] = float(vertexIndex % 2);
    position[(edgeAxis + 1) % 3] = float(sideIndex % 2);
    position[(edgeAxis + 2) % 3] = float(sideIndex / 2);
    return position;
}

OverlayVertex BoxVS(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    const Region region = regions[instanceIndex];
    const float3 lattice = (float3(region.offset) + BoxEdgeEndpoint(vertexIndex)) * float(regionVoxels);

    OverlayVertex output;
    output.position = mul(clipFromLattice, float4(lattice, 1.0));
    output.color = float4(BoxColor(region.state), 1.0);
    return output;
}

float4 OverlayPS(OverlayVertex input) : SV_Target
{
    return input.color;
}
