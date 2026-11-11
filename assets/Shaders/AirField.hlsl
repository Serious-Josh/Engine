// AIR.1 air field maintenance: initializing entering cells and building the packed obstacle mask from VX.5 residency; AIR.4D adds
// each cell's fluid pieces (Air.hlsli) from the same residency, one level finer, and (D2) carries retained cells' velocity over
// when their pieces are rebuilt.
// The kernels cover the whole grid but do work only for cells that need it, so recentring touches only entering slabs.
//
// They run back to back with no barrier between them (AirField::RecordMaintenance), which is safe only because no kernel reads
// what another writes: CSInitializeVelocity writes the velocity (and AIR.4F mist) of entering cells, CSBuildObstacles the mask
// words, and CSBuildPieces the piece masks of rebuilt cells and the velocity and mist of rebuilt cells that are not entering. Each
// CSBuildPieces thread reads its own cell's old masks, velocity and mist before overwriting them, so nothing it reads has been
// overwritten by another thread.

#include "Voxel.hlsli"
#include "Air.hlsli"

cbuffer AirFieldConstants : register(b0)
{
    VoxelWorldBinding voxels;
    AirFieldBinding field;
    // The previous minimum: cells outside [previousMinimum, previousMinimum + dimensions) are entering.
    int3 previousMinimum;
    // Every cell is entering (first frame, teleport, reset).
    uint fullReset;
    float3 ambientVelocity;
    // Every obstacle word is rebuilt (VX.5 residency changed).
    uint rebuildObstacles;
    // A cell is 2^voxelLevel voxels per axis; negative when voxels are larger than cells (the containing voxel is used).
    int voxelLevel;
    // Writable views (the binding publishes read-only ones). Scalars, not an array: cbuffer arrays pad every element to 16 bytes.
    uint velocityUnordered0;
    uint velocityUnordered1;
    uint obstacleUnordered;
    uint pieceUnordered;
    // AIR.4D D2: piece 1's velocity pair, and which buffer of each pair holds the latest velocity (0 or 1).
    uint piece1Unordered0;
    uint piece1Unordered1;
    uint currentVelocity;
    // AIR.4F: the mist pairs (piece 0's, then piece 1's), which of each pair is current, and whether mist is allocated (0: these
    // indices are not read).
    uint mistUnordered0;
    uint mistUnordered1;
    uint mistPiece1Unordered0;
    uint mistPiece1Unordered1;
    uint currentMist;
    uint mistEnabled;
    uint2 mistPadding;
};

bool Entering(int3 cell)
{
    const int3 previousMaximum = previousMinimum + int3(field.dimensions);
    return fullReset != 0 || any(cell < previousMinimum) || any(cell >= previousMaximum);
}

// All four velocity buffers (both pieces, both of each pair) of every entering cell start at the ambient velocity, before anything
// reads them. The step writes zero for a piece the cell turns out not to have.
[numthreads(8, 8, 8)]
void CSInitializeVelocity(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions) || !Entering(AirCellOfSlot(field, slot)))
    {
        return;
    }

    const uint address = AirVelocityAddress(field, slot);
    const uint4 ambient = asuint(float4(ambientVelocity, 0.0));
    const uint buffers[4] = {velocityUnordered0, velocityUnordered1, piece1Unordered0, piece1Unordered1};

    for(uint index = 0; index < 4; ++index)
    {
        RWByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(buffers[index])];
        velocity.Store4(address, ambient);
    }

    // AIR.4F: entering cells hold no mist (the window's outside is clear air).
    if(mistEnabled != 0)
    {
        const uint mistBuffers[4] = {mistUnordered0, mistUnordered1, mistPiece1Unordered0, mistPiece1Unordered1};

        for(uint index = 0; index < 4; ++index)
        {
            RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(mistBuffers[index])];
            mist.Store4(address, uint4(0, 0, 0, 0));
        }
    }
}

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

uint ObstacleBits(int3 cell)
{
    const VoxelSample sample = QueryLattice(cell, voxelLevel);
    return (sample.known ? AirObstacleKnown : 0u) | (sample.blocked ? AirObstacleBlocked : 0u);
}

// One thread per packed word (16 slots along x), so no atomics: unchanged slots keep their bits.
[numthreads(64, 1, 1)]
void CSBuildObstacles(uint3 thread : SV_DispatchThreadID)
{
    const uint wordsPerRow = field.dimensions.x / AirSlotsPerObstacleWord;
    const uint wordCount = wordsPerRow * field.dimensions.y * field.dimensions.z;
    const uint word = thread.x;

    if(word >= wordCount)
    {
        return;
    }

    const uint3 first = uint3((word % wordsPerRow) * AirSlotsPerObstacleWord, (word / wordsPerRow) % field.dimensions.y,
        word / (wordsPerRow * field.dimensions.y));
    RWByteAddressBuffer obstacles = ResourceDescriptorHeap[NonUniformResourceIndex(obstacleUnordered)];
    const uint previous = obstacles.Load(word * 4);
    uint bits = previous;
    bool changed = false;

    for(uint index = 0; index < AirSlotsPerObstacleWord; ++index)
    {
        const int3 cell = AirCellOfSlot(field, first + uint3(index, 0, 0));

        if(rebuildObstacles == 0 && !Entering(cell))
        {
            continue;
        }

        const uint shift = index * 2;
        bits = (bits & ~(3u << shift)) | (ObstacleBits(cell) << shift);
        changed = true;
    }

    if(changed)
    {
        obstacles.Store(word * 4, bits);
    }
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

// D2: the velocity a rebuilt piece starts with, from the old pieces of the same cell. It takes the velocity of the old piece it
// shares most sub-cells with (piece 0 on a tie), which carries velocity across a relabelling (pieces swapped or reshaped). A newly
// exposed piece, sharing nothing, starts at the ambient velocity; an absent piece is zero.
float3 RemappedVelocity(uint64_t piece, AirPieces previous, float3 previous0, float3 previous1)
{
    if(piece == 0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const uint overlap0 = AirSubcellCount(piece & previous.piece0);
    const uint overlap1 = AirSubcellCount(piece & previous.piece1);

    if(overlap0 == 0 && overlap1 == 0)
    {
        return ambientVelocity;
    }

    return overlap1 > overlap0 ? previous1 : previous0;
}

// AIR.4F: the mist a rebuilt piece starts with. Unlike velocity (a property of the air, carried whole), mist is an amount per
// volume: the new piece takes, from each old piece of the cell, that piece's mist over the sub-cells the two share, spread over the
// new piece's volume. Sub-cells newly exposed bring clear air, a piece sharing nothing starts clear, and no old piece gives more
// than its own volume's worth: a rebuild never creates mist, never moves it into a region it did not occupy, and leaves none in
// solid. An absent piece is zero.
float4 RemappedMist(uint64_t piece, AirPieces previous, float4 previous0, float4 previous1)
{
    const uint volume = AirSubcellCount(piece);

    if(volume == 0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float overlap0 = float(AirSubcellCount(piece & previous.piece0));
    const float overlap1 = float(AirSubcellCount(piece & previous.piece1));
    return (previous0 * overlap0 + previous1 * overlap1) / float(volume);
}

// One thread per cell; only cells the obstacle mask rebuilds (entering cells, or all after a VX.5 change) are written. A rebuilt
// cell that is not entering keeps its air: if its pieces changed, its velocity is remapped onto the new ones in the current buffer
// of each pair, which the next step reads (and whose other buffer it overwrites); unchanged cells are left as they are. Entering
// cells keep CSInitializeVelocity's ambient velocity.
[numthreads(8, 8, 8)]
void CSBuildPieces(uint3 slot : SV_DispatchThreadID)
{
    if(any(slot >= field.dimensions))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const bool entering = Entering(cell);

    if(rebuildObstacles == 0 && !entering)
    {
        return;
    }

    const AirPieces pieces = AirLabelPieces(FreeSubcells(cell));
    const uint address = AirVelocityAddress(field, slot);
    RWByteAddressBuffer output = ResourceDescriptorHeap[NonUniformResourceIndex(pieceUnordered)];
    const uint4 rebuilt = uint4(uint(pieces.piece0), uint(pieces.piece0 >> 32), uint(pieces.piece1), uint(pieces.piece1 >> 32));

    if(!entering)
    {
        // Read everything this cell had before writing any of it.
        const uint4 words = output.Load4(address);

        if(all(words == rebuilt))
        {
            return;
        }

        AirPieces previous;
        previous.piece0 = uint64_t(words.x) | (uint64_t(words.y) << 32);
        previous.piece1 = uint64_t(words.z) | (uint64_t(words.w) << 32);
        RWByteAddressBuffer current0 = ResourceDescriptorHeap[NonUniformResourceIndex(currentVelocity == 0 ? velocityUnordered0 : velocityUnordered1)];
        RWByteAddressBuffer current1 = ResourceDescriptorHeap[NonUniformResourceIndex(currentVelocity == 0 ? piece1Unordered0 : piece1Unordered1)];
        const float3 previous0 = asfloat(current0.Load3(address));
        const float3 previous1 = asfloat(current1.Load3(address));
        current0.Store4(address, asuint(float4(RemappedVelocity(pieces.piece0, previous, previous0, previous1), 0.0)));
        current1.Store4(address, asuint(float4(RemappedVelocity(pieces.piece1, previous, previous0, previous1), 0.0)));

        if(mistEnabled != 0)
        {
            RWByteAddressBuffer mist0 = ResourceDescriptorHeap[NonUniformResourceIndex(currentMist == 0 ? mistUnordered0 : mistUnordered1)];
            RWByteAddressBuffer mist1 = ResourceDescriptorHeap[NonUniformResourceIndex(currentMist == 0 ? mistPiece1Unordered0 : mistPiece1Unordered1)];
            const float4 previousMist0 = asfloat(mist0.Load4(address));
            const float4 previousMist1 = asfloat(mist1.Load4(address));
            mist0.Store4(address, asuint(RemappedMist(pieces.piece0, previous, previousMist0, previousMist1)));
            mist1.Store4(address, asuint(RemappedMist(pieces.piece1, previous, previousMist0, previousMist1)));
        }
    }

    output.Store4(address, rebuilt);
}
