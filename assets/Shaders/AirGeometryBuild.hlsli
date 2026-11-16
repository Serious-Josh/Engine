// AIR geometry building shared by maintenance (AirField.hlsl) and the AIR.4F.3 F1a moving-solid overlay (AirMovingSolids.hlsl): each
// cell's free sub-cells from VX.5 residency, and each slot's face connections from the pieces. Moved here unchanged from AirField.hlsl.
//
// The including shader declares, in its constants: `VoxelWorldBinding voxels`, `AirFieldBinding field`, `int voxelLevel` and
// `uint pieceUnordered` (the writable piece masks, read here through that view: the state the buffer is in while geometry is built).

#ifndef AIR_GEOMETRY_BUILD_HLSLI
#define AIR_GEOMETRY_BUILD_HLSLI

// Conservative: a lattice cell `level` levels above the voxels is blocked when any voxel it covers is known blocked
// (VoxelQueryCell is the exact OR); below the voxels (negative level) it takes the containing voxel. An invalid VX.5 binding
// answers unknown, which is free air.
VoxelSample QueryLattice(int3 cell, int level)
{
    if(level >= 0)
    {
        return VoxelQueryCell(voxels, cell, level);
    }

    return VoxelQueryVoxel(voxels, cell >> (-level));
}

// A cell's static obstacle bits: known when the residency has data for it, blocked when any voxel it covers is known blocked. A cell
// without the blocked bit is one whole free piece (FreeSubcells), which path consumers rely on (AirPath.hlsli, AirLoadCell).
uint ObstacleBits(int3 cell)
{
    const VoxelSample sample = QueryLattice(cell, voxelLevel);
    return (sample.known ? AirObstacleKnown : 0u) | (sample.blocked ? AirObstacleBlocked : 0u);
}

// The cell's free sub-cells: each free unless a voxel it covers is known blocked. A cell with no blocked voxel at all (known
// free or unknown) is free throughout, which settles nearly every cell with one query.
uint64_t FreeSubcells(int3 cell)
{
    if(!QueryLattice(cell, voxelLevel).blocked)
    {
        return AirSubcellsAll;
    }

    const int subcellLevel = voxelLevel - 2;
    uint64_t free = 0;

    for(uint bit = 0; bit < 64; ++bit)
    {
        const int3 subcell = cell * int(AirSubcellsPerAxis) + int3(bit & 3u, (bit >> 2) & 3u, bit >> 4);

        if(!QueryLattice(subcell, subcellLevel).blocked)
        {
            free |= uint64_t(1) << bit;
        }
    }

    return free;
}

// The pieces of a window cell, read through the writable view; outside the window, none.
AirPieces MaintainedPieces(int3 cell)
{
    AirPieces pieces;
    pieces.piece0 = 0;
    pieces.piece1 = 0;

    if(!AirContains(field, cell))
    {
        return pieces;
    }

    RWByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(pieceUnordered)];
    const uint4 words = buffer.Load4(AirVelocityAddress(field, AirSlotOf(field, cell)));
    pieces.piece0 = uint64_t(words.x) | (uint64_t(words.y) << 32);
    pieces.piece1 = uint64_t(words.z) | (uint64_t(words.w) << 32);
    return pieces;
}

// AIR.4E.1: a slot's face connections toward its +x, +y and +z neighbours (Air.hlsli, AirApertureOf), from the pieces as they are now.
// A record depends on the slot's own pieces and on its three upper neighbours' pieces.
uint64_t AirBuildApertureRecord(uint3 slot)
{
    const int3 cell = AirCellOfSlot(field, slot);
    const AirPieces own = MaintainedPieces(cell);
    const uint64_t masks[2] = {own.piece0, own.piece1};
    uint64_t record = 0;

    for(uint axis = 0; axis < 3; ++axis)
    {
        int3 next = cell;
        next[axis] += 1;
        const bool outside = !AirContains(field, next);
        const AirPieces neighbour = MaintainedPieces(next);
        const uint64_t neighbours[2] = {neighbour.piece0, neighbour.piece1};

        for(uint lower = 0; lower < 2; ++lower)
        {
            for(uint upper = 0; upper < 2; ++upper)
            {
                uint aperture = 0;

                if(masks[lower] != 0)
                {
                    aperture = outside ? (upper == 0 ? AirHighBoundaryAperture(masks[lower], axis) : 0u) :
                        AirPieceLink(masks[lower], neighbours[upper], axis);
                }

                record |= uint64_t(aperture) << ((axis * 4u + lower * 2u + upper) * 5u);
            }
        }

        if(outside)
        {
            record |= uint64_t(1) << (60u + axis);
        }
    }

    return record;
}

#endif
