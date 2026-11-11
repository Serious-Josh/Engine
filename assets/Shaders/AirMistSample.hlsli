// AIR.4F: piece-aware sampling of the passive mist field, by D2's rules (AirSample.hlsli): one piece per corner, never a blend of
// two pieces of a cell; corners left out are renormalized away. See Documentation/Air/AirArchitecture.md, AIR.4F.
//
// Mist is premultiplied: per fluid piece, a float4 of (density-weighted colour, density), in the velocity buffers' layout
// (AirVelocityAddress). Every operation on it (interpolation, dissipation, rebuild remapping) is linear in all four components
// together, so colour stays tied to density. Outside the window the mist is zero (clear air).

#ifndef AIR_MIST_SAMPLE_HLSLI
#define AIR_MIST_SAMPLE_HLSLI

#include "AirSample.hlsli"

// The mist buffers a sample reads (descriptor indices, of the path's view kind).
struct AirMistField
{
    uint piece0Index;
    uint piece1Index;
};

// A piece's mist; outside the window, zero.
float4 AirPieceMist(AirPathField path, AirMistField mist, int3 cell, uint piece)
{
    if(!AirContains(path.field, cell))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    AirPathBuffer values = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? mist.piece0Index : mist.piece1Index)];
    return asfloat(values.Load4(AirVelocityAddress(path.field, AirSlotOf(path.field, cell))));
}

// Every corner open (whole cells, or outside the window): plain trilinear over piece 0, the outside counting as clear air.
float4 AirSampleMistOpen(AirPathField path, AirMistField mist, int3 base, float3 fraction, float4 fallback)
{
    float4 sum = 0.0;
    float total = 0.0;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;

        if(weight > 0.0)
        {
            sum += AirPieceMist(path, mist, path.field.minimum + base + offset, 0) * weight;
            total += weight;
        }
    }

    return total > 0.0 ? sum / total : fallback;
}

// Beside a split cell: the corners and pieces AirChooseCorners picks, exactly as the velocity sample does.
float4 AirSampleMistSplit(AirPathField path, AirMistField mist, AirPathEnd at, int3 base, float3 fraction, float4 fallback)
{
    const AirCornerChoice choice = AirChooseCorners(path, at, base, fraction);
    float4 sum = 0.0;
    float total = 0.0;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);

        if(choice.piece[corner] != AirNoPiece)
        {
            sum += AirPieceMist(path, mist, path.field.minimum + base + offset, choice.piece[corner]) * choice.weight[corner];
            total += choice.weight[corner];
        }
    }

    return total > 0.0 ? sum / total : fallback;
}

// The mist at a backtrace's end; `fallback` (the piece's own mist) where no corner is left.
float4 AirSampleMist(AirPathField path, AirMistField mist, AirPathEnd at, float4 fallback)
{
    const float3 shifted = at.position - 0.5;
    const int3 base = int3(floor(shifted));
    const float3 fraction = shifted - float3(base);

    if(!AirCornersOpen(path, base))
    {
        return AirSampleMistSplit(path, mist, at, base, fraction, fallback);
    }

    return AirSampleMistOpen(path, mist, base, fraction, fallback);
}

#endif
