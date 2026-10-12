// Root constants: one 16-byte register, in the CPU binding order.
cbuffer DisplacementIndicesConstants : register(b0)
{
    uint patchesPerAxis;
    uint subdivisionsPerPatch;
    uint patchBufferIndex;
    uint indexBufferIndex;
};

void EmitIndex(RWByteAddressBuffer output, bool writeEnabled,
    inout uint cursor, uint endIndex, uint value)
{
    // Both passes use the identical emission path. The guard also protects each store.
    if(writeEnabled && cursor < endIndex)
        output.Store(cursor * 4u, value);
    ++cursor;
}

uint CornerIndex(uint originX, uint originZ, uint dx, uint dz,
    uint vertexStep, uint rowStride, bool negativeX, bool negativeZ)
{
    const uint x = negativeX ? originX + dx * vertexStep : originX - dx * vertexStep;
    const uint z = negativeZ ? originZ + dz * vertexStep : originZ - dz * vertexStep;
    return x * rowStride + z;
}

uint EmitPatch(RWByteAddressBuffer output, bool writeEnabled, uint firstIndex,
    uint endIndex, uint patchX, uint patchZ, uint subdivisions, uint vertexStep,
    uint gridResolution, uint flags)
{
    uint cursor = firstIndex;
    const uint rowStride = gridResolution + 1u;
    const bool stitchNegativeX = (flags & 1u) != 0u;
    const bool stitchPositiveX = (flags & 2u) != 0u;
    const bool stitchNegativeZ = (flags & 4u) != 0u;
    const bool stitchPositiveZ = (flags & 8u) != 0u;
    for(uint localX = 0; localX < subdivisions; ++localX)
    {
        const uint x =
            patchX * subdivisionsPerPatch +
            localX * vertexStep;

        for(uint localZ = 0; localZ < subdivisions; ++localZ)
        {
            const uint z =
                patchZ * subdivisionsPerPatch +
                localZ * vertexStep;

            // Reserve a 2x2-cell block where two transition strips meet.
            // Both coarse-facing edge midpoints are omitted; the other
            // boundary vertices remain shared with the ordinary cells.
            const bool cornerNegativeX = stitchNegativeX && localX < 2u;
            const bool cornerPositiveX = stitchPositiveX && localX >= subdivisions - 2u;
            const bool cornerNegativeZ = stitchNegativeZ && localZ < 2u;
            const bool cornerPositiveZ = stitchPositiveZ && localZ >= subdivisions - 2u;
            if((cornerNegativeX || cornerPositiveX) &&
               (cornerNegativeZ || cornerPositiveZ))
            {
                const uint blockX = cornerNegativeX ? 0u : subdivisions - 2u;
                const uint blockZ = cornerNegativeZ ? 0u : subdivisions - 2u;
                if(localX == blockX && localZ == blockZ)
                {
                    const uint originX = patchX * subdivisionsPerPatch +
                        (cornerNegativeX ? 0u : subdivisions) * vertexStep;
                    const uint originZ = patchZ * subdivisionsPerPatch +
                        (cornerNegativeZ ? 0u : subdivisions) * vertexStep;
                    
                    const uint center = CornerIndex(originX, originZ, 1u, 1u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ);
                    const uint boundary[] = {
                        CornerIndex(originX, originZ, 0u, 0u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ), CornerIndex(originX, originZ, 0u, 2u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ),
                        CornerIndex(originX, originZ, 1u, 2u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ), CornerIndex(originX, originZ, 2u, 2u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ),
                        CornerIndex(originX, originZ, 2u, 1u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ), CornerIndex(originX, originZ, 2u, 0u, vertexStep, rowStride, cornerNegativeX, cornerNegativeZ)};
                    for(uint edge = 0; edge < 6u; ++edge)
                    {
                        const uint next = (edge + 1u) % 6u;
                        EmitIndex(output, writeEnabled, cursor, endIndex, center);
                        EmitIndex(output, writeEnabled, cursor, endIndex, boundary[cornerNegativeX == cornerNegativeZ ? edge : next]);
                        EmitIndex(output, writeEnabled, cursor, endIndex, boundary[cornerNegativeX == cornerNegativeZ ? next : edge]);
                    }
                }
                continue;
            }

            // Stitch the positive-X edge against a patch one LOD coarser.
            if(stitchPositiveX &&
            localX == subdivisions - 1 &&
            (localZ % 2u) == 0u)
            {
                const uint innerX = x;
                const uint outerX = x + vertexStep;

                const uint z0 = z;
                const uint z1 = z + vertexStep;
                const uint z2 = z + 2u * vertexStep;

                const uint i0 = (innerX * rowStride + z0);
                const uint i1 = (innerX * rowStride + z1);
                const uint i2 = (innerX * rowStride + z2);

                const uint o0 = (outerX * rowStride + z0);
                const uint o2 = (outerX * rowStride + z2);

                // Lower transition triangle.
                EmitIndex(output, writeEnabled, cursor, endIndex, i0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, o0);

                // Central transition triangle.
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);
                EmitIndex(output, writeEnabled, cursor, endIndex, o0);

                // Upper transition triangle.
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, i2);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);

                continue;
            }

            if(stitchPositiveX &&
            localX == subdivisions - 1 &&
            (localZ % 2u) == 1u)
            {
                // The preceding iteration already emitted this pair of cells.
                continue;
            }

            // Stitch the negative-X edge against a patch one LOD coarser.
            if(stitchNegativeX &&
            localX == 0u &&
            (localZ % 2u) == 0u)
            {
                const uint outerX = x;
                const uint innerX = x + vertexStep;

                const uint z0 = z;
                const uint z1 = z + vertexStep;
                const uint z2 = z + 2u * vertexStep;

                const uint o0 = (outerX * rowStride + z0);
                const uint o2 = (outerX * rowStride + z2);

                const uint i0 = (innerX * rowStride + z0);
                const uint i1 = (innerX * rowStride + z1);
                const uint i2 = (innerX * rowStride + z2);

                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);

                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, i0);

                EmitIndex(output, writeEnabled, cursor, endIndex, o2);
                EmitIndex(output, writeEnabled, cursor, endIndex, i2);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);

                continue;
            }

            if(stitchNegativeX &&
            localX == 0u &&
            (localZ % 2u) == 1u)
            {
                // The preceding iteration already emitted this pair of cells.
                continue;
            }

            // Stitch the negative-Z edge against a patch one LOD coarser.
            if(stitchNegativeZ &&
            localZ == 0u &&
            (localX % 2u) == 0u)
            {
                const uint zOuter = z;
                const uint zInner = z + vertexStep;

                const uint x0 = x;
                const uint x1 = x + vertexStep;
                const uint x2 = x + 2u * vertexStep;

                const uint o0 = (x0 * rowStride + zOuter);
                const uint o2 = (x2 * rowStride + zOuter);

                const uint i0 = (x0 * rowStride + zInner);
                const uint i1 = (x1 * rowStride + zInner);
                const uint i2 = (x2 * rowStride + zInner);

                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);

                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);

                EmitIndex(output, writeEnabled, cursor, endIndex, o2);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, i2);

                continue;
            }

            if(stitchNegativeZ &&
            localZ == 0u &&
            (localX % 2u) == 1u)
            {
                // The preceding X iteration already emitted these cells.
                continue;
            }

            // Stitch the positive-Z edge against a patch one LOD coarser.
            if(stitchPositiveZ &&
            localZ == subdivisions - 1 &&
            (localX % 2u) == 0u)
            {
                const uint zInner = z;
                const uint zOuter = z + vertexStep;

                const uint x0 = x;
                const uint x1 = x + vertexStep;
                const uint x2 = x + 2u * vertexStep;

                const uint i0 = (x0 * rowStride + zInner);
                const uint i1 = (x1 * rowStride + zInner);
                const uint i2 = (x2 * rowStride + zInner);

                const uint o0 = (x0 * rowStride + zOuter);
                const uint o2 = (x2 * rowStride + zOuter);

                EmitIndex(output, writeEnabled, cursor, endIndex, i0);
                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, i1);

                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, o0);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);

                EmitIndex(output, writeEnabled, cursor, endIndex, i1);
                EmitIndex(output, writeEnabled, cursor, endIndex, o2);
                EmitIndex(output, writeEnabled, cursor, endIndex, i2);

                continue;
            }

            if(stitchPositiveZ &&
            localZ == subdivisions - 1 &&
            (localX % 2u) == 1u)
            {
                // The preceding X iteration already emitted these cells.
                continue;
            }

            const uint a =
                x * (gridResolution + 1) + z;

            const uint b = a + vertexStep;

            const uint c =
                b + vertexStep * (gridResolution + 1);

            const uint d =
                a + vertexStep * (gridResolution + 1);

            EmitIndex(output, writeEnabled, cursor, endIndex, a);
            EmitIndex(output, writeEnabled, cursor, endIndex, b);
            EmitIndex(output, writeEnabled, cursor, endIndex, c);

            EmitIndex(output, writeEnabled, cursor, endIndex, a);
            EmitIndex(output, writeEnabled, cursor, endIndex, c);
            EmitIndex(output, writeEnabled, cursor, endIndex, d);
        }
    }

    return cursor - firstIndex;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // Validate products before multiplying; raw buffer offsets are 32-bit bytes.
    if(patchesPerAxis == 0u || subdivisionsPerPatch < 2u ||
        (subdivisionsPerPatch & (subdivisionsPerPatch - 1u)) != 0u)
        return;
    if(patchesPerAxis > (0xffffffffu / 32u) / patchesPerAxis ||
        subdivisionsPerPatch > 0xfffffffeu / patchesPerAxis)
        return;
    const uint patchCount = patchesPerAxis * patchesPerAxis;
    const uint patchID = dispatchThreadID.x;
    if(patchID >= patchCount || dispatchThreadID.y != 0u || dispatchThreadID.z != 0u)
        return;
    const uint gridResolution = patchesPerAxis * subdivisionsPerPatch;
    const uint rowStride = gridResolution + 1u;
    // Match the layout builder's 48-byte vertex-buffer bound. This also bounds
    // vertex indices and the maximum emission count (six indices per cell).
    if(rowStride > (0xffffffffu / 48u) / rowStride)
        return;

    ByteAddressBuffer patches = ResourceDescriptorHeap[patchBufferIndex];
    RWByteAddressBuffer output = ResourceDescriptorHeap[indexBufferIndex];
    uint patchBytes, indexBytes;
    patches.GetDimensions(patchBytes);
    output.GetDimensions(indexBytes);
    if(patchBytes / 32u < patchCount)
        return;
    const uint4 record0 = patches.Load4(patchID * 32u);
    const uint4 record1 = patches.Load4(patchID * 32u + 16u);
    const uint patchX = record0.x;
    const uint patchZ = record0.y;
    const uint lod = record0.z;
    const uint firstIndex = record0.w;
    const uint indexCount = record1.x;
    const uint flags = record1.y;
    if(patchX != patchID / patchesPerAxis || patchZ != patchID % patchesPerAxis ||
        lod > 1u || (flags & ~15u) != 0u || (lod != 0u && flags != 0u) ||
        record1.z != 0u || record1.w != 0u)
        return;
    if(((flags & 1u) != 0u && patchX == 0u) ||
        ((flags & 2u) != 0u && patchX + 1u == patchesPerAxis) ||
        ((flags & 4u) != 0u && patchZ == 0u) ||
        ((flags & 8u) != 0u && patchZ + 1u == patchesPerAxis))
        return;
    const uint capacity = indexBytes / 4u;
    if(firstIndex > capacity || indexCount > capacity - firstIndex || indexCount % 3u != 0u)
        return;
    const uint subdivisions = subdivisionsPerPatch >> lod;
    const uint vertexStep = 1u << lod;
    const uint endIndex = firstIndex + indexCount;
    // Count before writing: reject incorrect counts even for overlapping corner
    // blocks at subdivisions == 2, where a simple area formula is insufficient.
    const uint expected = EmitPatch(output, false, 0u, 0u, patchX, patchZ,
        subdivisions, vertexStep, gridResolution, flags);
    if(expected != indexCount)
        return;
    EmitPatch(output, true, firstIndex, endIndex, patchX, patchZ,
        subdivisions, vertexStep, gridResolution, flags);
}
