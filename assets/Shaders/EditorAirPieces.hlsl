// AIR.4D Editor "Fluid pieces" diagnostic: the boundary the air's fluid pieces see, at sub-cell resolution. One instance per
// sub-cell of a box (EditorAirPass.h), 36 vertices each. A solid sub-cell draws each face that borders a free sub-cell, turned
// towards it and coloured by what that free sub-cell belongs to: piece 0, piece 1, or a further piece the representation drops.
//
// The pieces come from the published masks (Air.hlsli); whether a sub-cell in neither mask is solid or a dropped piece is told
// by querying VX.5 for that sub-cell, exactly as the mask build does. Nothing is read back to the CPU.

#include "Voxel.hlsli"
#include "Air.hlsli"
#include "EditorCubeFaces.hlsli"

cbuffer EditorAirPieceConstants : register(b0)
{
    row_major float4x4 viewProjection;
    AirFieldBinding field;
    VoxelWorldBinding voxels;
    // The box's first sub-cell and its size in sub-cells.
    int3 firstSubcell;
    // Sub-cells are 2^subcellLevel voxels per axis (negative: smaller than a voxel), as in CSBuildPieces.
    int subcellLevel;
    uint3 subcells;
    uint padding;
};

static const uint SubcellSolid = 0;
static const uint SubcellPiece0 = 1;
static const uint SubcellPiece1 = 2;
static const uint SubcellDropped = 3;
static const uint SubcellOutside = 4;

struct PieceVertex
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
    float2 faceCoordinate : TEXCOORD0;
};

uint SubcellState(int3 subcell)
{
    // Arithmetic shifts floor negative coordinates.
    const int3 cell = subcell >> 2;

    if(!AirContains(field, cell))
    {
        return SubcellOutside;
    }

    const int3 local = subcell - cell * int(AirSubcellsPerAxis);
    const uint64_t bit = uint64_t(1) << uint(local.x + 4 * local.y + 16 * local.z);
    const AirPieces pieces = AirPiecesOf(field, cell);

    if((pieces.piece0 & bit) != 0)
    {
        return SubcellPiece0;
    }

    if((pieces.piece1 & bit) != 0)
    {
        return SubcellPiece1;
    }

    // As CSBuildPieces: the OR of the voxels a sub-cell covers, or the voxel containing it.
    VoxelSample sample;

    if(subcellLevel >= 0)
    {
        sample = VoxelQueryCell(voxels, subcell, subcellLevel);
    }
    else
    {
        sample = VoxelQueryVoxel(voxels, subcell >> (-subcellLevel));
    }

    return sample.blocked ? SubcellSolid : SubcellDropped;
}

float3 StateColor(uint state)
{
    if(state == SubcellPiece0)
    {
        return float3(0.15, 0.8, 0.75);
    }

    if(state == SubcellPiece1)
    {
        return float3(1.0, 0.85, 0.1);
    }

    return float3(1.0, 0.15, 0.85);
}

PieceVertex VSMain(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
{
    const int3 subcell = firstSubcell + int3(instance % subcells.x, (instance / subcells.x) % subcells.y, instance / (subcells.x * subcells.y));
    const uint face = vertexIndex / 6;
    int3 across = subcell;
    across[face / 2] += (face % 2) == 1 ? 1 : -1;
    const bool solid = field.valid != 0 && SubcellState(subcell) == SubcellSolid;
    const uint neighbour = solid ? SubcellState(across) : SubcellOutside;
    const bool drawn = neighbour == SubcellPiece0 || neighbour == SubcellPiece1 || neighbour == SubcellDropped;

    float2 uv;
    const float3 corner = CubeFaceCorner(face, vertexIndex % 6, uv);
    // Integer sub-cell plus a 0/1 corner, times a power-of-two size: the face lies exactly on the sub-cell's bounds.
    const float3 world = (float3(subcell) + corner) * (field.cellSize / float(AirSubcellsPerAxis));

    PieceVertex output;
    // A face not drawn is placed in front of the near plane, so its triangles are clipped entirely.
    output.position = drawn ? mul(viewProjection, float4(world, 1.0)) : float4(0.0, 0.0, -1.0, 1.0);
    output.color = float4(StateColor(neighbour) * CubeFaceShade(face), 1.0);
    output.faceCoordinate = uv;
    return output;
}

float4 PSMain(PieceVertex input) : SV_Target
{
    return OutlinedFace(input.color.rgb, input.faceCoordinate);
}
