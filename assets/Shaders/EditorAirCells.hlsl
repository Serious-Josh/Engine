// AIR.4C Editor air-cell diagnostic: the air field's own obstacle mask, drawn as the exposed faces of blocked (and optionally
// unknown) cells at their exact world bounds, so the mask can be compared with the visible geometry. Reads the published mask
// through AirFieldBinding; nothing is read back to the CPU.
//
// One instance per cell of a box (EditorAirPass.h); 36 vertices per instance (six faces of two triangles). A face is drawn when
// its cell is shown and the cell across it is in a different state (or outside the window). Front faces are clockwise from
// outside, as for every Engine mesh (EditorVoxel.hlsl), so only faces turned towards the camera draw.

#include "Air.hlsli"
#include "EditorCubeFaces.hlsli"

cbuffer EditorAirCellConstants : register(b0)
{
    row_major float4x4 viewProjection;
    AirFieldBinding field;
    // The box's first cell and its size in cells.
    int3 firstCell;
    // Bit 0: blocked cells; bit 1: unknown cells.
    uint showMask;
    uint3 cells;
    uint padding;
};

static const uint CellKnownFree = 0;
static const uint CellBlocked = 1;
static const uint CellUnknown = 2;
static const uint CellOutside = 3;

struct CellVertex
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
    float2 faceCoordinate : TEXCOORD0;
};

uint CellState(int3 cell)
{
    if(!AirContains(field, cell))
    {
        return CellOutside;
    }

    const uint bits = AirObstacle(field, cell);
    return (bits & AirObstacleBlocked) != 0 ? CellBlocked : ((bits & AirObstacleKnown) != 0 ? CellKnownFree : CellUnknown);
}

bool Shown(uint state)
{
    return (state == CellBlocked && (showMask & 1u) != 0) || (state == CellUnknown && (showMask & 2u) != 0);
}

CellVertex VSMain(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
{
    const int3 cell = firstCell + int3(instance % cells.x, (instance / cells.x) % cells.y, instance / (cells.x * cells.y));
    const uint face = vertexIndex / 6;
    const uint state = CellState(cell);
    int3 across = cell;
    across[face / 2] += (face % 2) == 1 ? 1 : -1;
    const bool drawn = field.valid != 0 && Shown(state) && CellState(across) != state;

    float2 uv;
    const float3 corner = CubeFaceCorner(face, vertexIndex % 6, uv);
    // Integer cell plus a 0/1 corner times a power-of-two cell size: the face lies exactly on the cell's bounds.
    const float3 world = (float3(cell) + corner) * field.cellSize;
    const float3 color = state == CellBlocked ? float3(1.0, 0.35, 0.15) : float3(0.55, 0.55, 0.85);

    CellVertex output;
    // A face not drawn is placed in front of the near plane, so its triangles are clipped entirely.
    output.position = drawn ? mul(viewProjection, float4(world, 1.0)) : float4(0.0, 0.0, -1.0, 1.0);
    output.color = float4(color * CubeFaceShade(face), 1.0);
    output.faceCoordinate = uv;
    return output;
}

float4 PSMain(CellVertex input) : SV_Target
{
    return OutlinedFace(input.color.rgb, input.faceCoordinate);
}
