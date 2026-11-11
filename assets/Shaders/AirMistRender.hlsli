// AIR.4F F3: geometry-safe mist density (and, F4.1, velocity) for rendering, shared by the Editor's mist pass (EditorMist.hlsl) and
// its validation. See Documentation/Air/AirArchitecture.md, AIR.4F F3 and F4.2.
//
// A point's mist is read straight from F1's buffers (no separate render copy), from the fluid piece holding it and the pieces
// connected to it:
//   - outside the window, or in a sub-cell no piece holds (solid voxel geometry, or a piece D1 drops): exactly zero;
//   - where all 8 cells whose centres surround the point are whole (one piece filling each; unknown space is whole) or outside the
//     window: plain trilinear over piece 0, the outside counting as clear air (whole neighbours meet through whole open faces);
//   - anywhere else (F4.2): trilinear over the corners connected to the point's own piece. AirSearchCorners (D2's link search) walks
//     the 8 cells' pieces through shared open sub-faces (AirPieceLink), so a corner counts only when sub-cell connectivity within the
//     footprint joins it to the point's piece; a corner reachable through both of its pieces takes the one with fewer links, then the
//     nearer sub-cell (D2's tie rule); an unreachable corner is left out and the weights renormalized; with none left, the holding
//     piece's own value. A corner merely being fluid is never enough.
// This is D2's corner rule without its first tier (the straight-path traversal per corner), which is the costly part: a corner joined
// only by a straight line through open sub-cells, but by no link path inside the footprint, is left out. Every choice is deterministic
// and every value is a convex blend of premultiplied pieces, so colour stays tied to density.

#ifndef AIR_MIST_RENDER_HLSLI
#define AIR_MIST_RENDER_HLSLI

#include "AirMistSample.hlsli"

// The corners a render sample blends near geometry: each corner's weight and piece (AirNoPiece: left out; outside the window: piece 0,
// read as clear air).
AirCornerChoice AirRenderCorners(AirPathField path, float3 position, int3 cell, uint piece, int3 base, float3 fraction)
{
    AirCell corners[8];

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        corners[corner] = AirLoadCell(path, path.field.minimum + base + offset);
    }

    // The point's own cell is always one of the 8 (base is the cell holding the point's position minus half a cell).
    const int3 home = cell - path.field.minimum - base;
    const uint homeCorner = uint(home.x + 2 * home.y + 4 * home.z);
    uint hops[16];
    AirSearchCorners(homeCorner, piece, corners, hops);
    AirCornerChoice choice;

    for(uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
        const float weight = axisWeight.x * axisWeight.y * axisWeight.z;
        choice.piece[corner] = AirNoPiece;
        choice.weight[corner] = 0.0;

        if(weight <= 0.0)
        {
            continue;
        }

        choice.weight[corner] = weight;

        if(!corners[corner].inside)
        {
            choice.piece[corner] = 0;
            continue;
        }

        if(corner == homeCorner)
        {
            choice.piece[corner] = piece;
            continue;
        }

        const uint hops0 = hops[corner * 2];
        const uint hops1 = hops[corner * 2 + 1];

        if(hops0 == 255 && hops1 == 255)
        {
            continue;
        }

        if(hops0 != hops1)
        {
            choice.piece[corner] = hops0 < hops1 ? 0u : 1u;
            continue;
        }

        const int3 local = base + offset;
        const float distance0 = AirNearestSubcellDistance(position, local, corners[corner].piece0);
        const float distance1 = AirNearestSubcellDistance(position, local, corners[corner].piece1);
        choice.piece[corner] = distance1 < distance0 ? 1u : 0u;
    }

    return choice;
}

// AIR.4F F4.3: no sun visibility buffer (AirRenderMistAndVelocity's visibilityIndex): every sample sees the sun.
static const uint AirNoVisibility = 0xFFFFFFFFu;

// AIR.4F F4.3: a piece's sun visibility (0 or 1) from the visibility buffer (EditorMist.hlsl, CSSunVisibility): one uint per cell
// slot, bit `piece` set when the ray from the piece's anchor toward the sun reached it.
float AirPieceSunVisibility(AirPathField path, uint visibilityIndex, int3 cell, uint piece)
{
    ByteAddressBuffer visibility = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityIndex)];
    const uint bits = visibility.Load(AirVelocityAddress(path.field, AirSlotOf(path.field, cell)) / 4);
    return float((bits >> piece) & 1u);
}

// AIR.4F F4.1: the mist at a point and, when asked for, the air's velocity there by the same rule, so flow-advected detail follows the
// air connected to the point and never the air beyond a wall. AIR.4F F4.3: likewise, with a visibility buffer, the sun's visibility:
// the same corners and weights as the mist, so it is blended only between connected pieces; outside the window, a corner takes the
// point's own piece's visibility.
struct AirRenderSample
{
    float4 mist;
    float3 velocity;
    float sunVisibility;
};

AirRenderSample AirRenderMistAndVelocity(AirPathField path, AirMistField mist, float3 position, bool withVelocity,
    uint visibilityIndex = AirNoVisibility)
{
    AirRenderSample result;
    result.mist = float4(0.0, 0.0, 0.0, 0.0);
    result.velocity = float3(0.0, 0.0, 0.0);
    result.sunVisibility = 1.0;
    const bool withVisibility = visibilityIndex != AirNoVisibility;
    const int3 cell = path.field.minimum + int3(floor(position));
    const AirCell holder = AirLoadCell(path, cell);

    if(!holder.inside)
    {
        return result;
    }

    const int3 subcell = clamp(int3(floor(position * 4.0)) - (cell - path.field.minimum) * 4, 0, 3);
    const uint piece = AirPieceOfSubcell(holder, subcell);

    if(piece == AirNoPiece)
    {
        return result;
    }

    const float3 shifted = position - 0.5;
    const int3 base = int3(floor(shifted));
    const float3 fraction = shifted - float3(base);
    const float4 own = AirPieceMist(path, mist, cell, piece);
    const float ownVisibility = withVisibility ? AirPieceSunVisibility(path, visibilityIndex, cell, piece) : 1.0;

    // The fast path: an open footprint is plain trilinear. HLSL evaluates both sides of a conditional expression, so branches guard
    // each path.
    if(AirCornersOpen(path, base))
    {
        result.mist = AirSampleMistOpen(path, mist, base, fraction, own);

        if(withVelocity && result.mist.a > 0.0)
        {
            result.velocity = AirSampleOpen(path, base, fraction, AirPieceVelocity(path, cell, piece));
        }

        if(withVisibility && result.mist.a > 0.0)
        {
            float visibilitySum = 0.0;
            float visibilityTotal = 0.0;

            for(uint corner = 0; corner < 8; ++corner)
            {
                const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
                const float3 axisWeight = lerp(1.0 - fraction, fraction, float3(offset));
                const float weight = axisWeight.x * axisWeight.y * axisWeight.z;
                const int3 cornerCell = path.field.minimum + base + offset;

                if(weight > 0.0)
                {
                    const bool inside = AirContains(path.field, cornerCell);
                    visibilitySum += (inside ? AirPieceSunVisibility(path, visibilityIndex, cornerCell, 0) : ownVisibility) * weight;
                    visibilityTotal += weight;
                }
            }

            result.sunVisibility = visibilityTotal > 0.0 ? visibilitySum / visibilityTotal : ownVisibility;
        }

        return result;
    }

    const AirCornerChoice choice = AirRenderCorners(path, position, cell, piece, base, fraction);
    float4 mistSum = float4(0.0, 0.0, 0.0, 0.0);
    float3 velocitySum = float3(0.0, 0.0, 0.0);
    float visibilitySum = 0.0;
    float total = 0.0;

    for(uint corner = 0; corner < 8; ++corner)
    {
        if(choice.piece[corner] == AirNoPiece)
        {
            continue;
        }

        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const int3 cornerCell = path.field.minimum + base + offset;
        mistSum += AirPieceMist(path, mist, cornerCell, choice.piece[corner]) * choice.weight[corner];

        // Outside the window there is no air to follow (the render path's ambient is zero).
        if(withVelocity && AirContains(path.field, cornerCell))
        {
            velocitySum += AirPieceVelocity(path, cornerCell, choice.piece[corner]) * choice.weight[corner];
        }

        if(withVisibility)
        {
            const bool inside = AirContains(path.field, cornerCell);
            visibilitySum += (inside ? AirPieceSunVisibility(path, visibilityIndex, cornerCell, choice.piece[corner]) : ownVisibility) *
                choice.weight[corner];
        }

        total += choice.weight[corner];
    }

    result.mist = total > 0.0 ? mistSum / total : own;

    if(withVelocity && result.mist.a > 0.0)
    {
        result.velocity = total > 0.0 ? velocitySum / total : AirPieceVelocity(path, cell, piece);
    }

    if(withVisibility && result.mist.a > 0.0)
    {
        result.sunVisibility = total > 0.0 ? visibilitySum / total : ownVisibility;
    }

    return result;
}

// AIR.4F F4.1: the two flow phases of the Worley detail (EditorMistPass::DetailPhasesAt).
struct AirMistDetail
{
    float3 offsetA;
    float strength;
    float3 offsetB;
    // 1 / the noise tile's size in metres.
    float inverseScale;
    // Seconds since each phase restarted, and phase A's weight (phase B's is 1 - it).
    float timeA;
    float timeB;
    float weightA;
    // 1 / the texture's mean, so noise / mean averages 1.
    float meanInverse;
};

// The detail's density factor at a world point carried by `velocity`: lerp(1, noise / mean, strength), where noise blends the two
// phases' samples at (world - velocity * tau) / scale + offset. `valueA` and `valueB` return each phase's raw sample.
float AirMistDetailFactor(Texture3D<float> noise, SamplerState noiseSampler, AirMistDetail detail, float3 world, float3 velocity,
    out float valueA, out float valueB)
{
    valueA = noise.SampleLevel(noiseSampler, (world - velocity * detail.timeA) * detail.inverseScale + detail.offsetA, 0.0);
    valueB = noise.SampleLevel(noiseSampler, (world - velocity * detail.timeB) * detail.inverseScale + detail.offsetB, 0.0);
    const float value = detail.weightA * valueA + (1.0 - detail.weightA) * valueB;
    return lerp(1.0, value * detail.meanInverse, detail.strength);
}

// AIR.4F F4.4: curl-warped Worley detail (EditorMist.hlsl's curl constants). Each phase's Worley coordinate is moved by the curl field
// before it is looked up: world + strength x C(curl coordinate), where C is the tileable divergence-free field (unit RMS length after
// rmsInverse) sampled at (world - flow x velocity x tau) / scale + the phase's offset. The curl pattern thus rides the local air like
// the Worley pattern (flow 1) and restarts with its phase, unseen; only the detail's lookup moves, never the mist's density.
struct AirMistCurl
{
    // Metres of displacement per unit of the field's RMS length (strength x rmsInverse), 1 / the curl tile's size in metres, and how
    // far the curl pattern follows the air (0: fixed in the world, 1: with the air).
    float displacement;
    float inverseScale;
    float flow;
    float curlPadding;
};

// One phase's Worley coordinate, warped by the curl field.
float3 AirCurledDetailCoordinate(Texture3D<float4> curl, SamplerState noiseSampler, AirMistDetail detail, AirMistCurl curlField, float3 world,
    float3 velocity, float time, float3 offset)
{
    const float3 curlCoordinate = (world - velocity * (curlField.flow * time)) * curlField.inverseScale + offset;
    const float3 warp = curl.SampleLevel(noiseSampler, curlCoordinate, 0.0).xyz * curlField.displacement;
    return (world + warp - velocity * time) * detail.inverseScale + offset;
}

// AirMistDetailFactor with each phase's Worley coordinate warped by the curl field.
float AirMistCurledDetailFactor(Texture3D<float> noise, Texture3D<float4> curl, SamplerState noiseSampler, AirMistDetail detail,
    AirMistCurl curlField, float3 world, float3 velocity, out float valueA, out float valueB)
{
    valueA = noise.SampleLevel(noiseSampler, AirCurledDetailCoordinate(curl, noiseSampler, detail, curlField, world, velocity, detail.timeA,
        detail.offsetA), 0.0);
    valueB = noise.SampleLevel(noiseSampler, AirCurledDetailCoordinate(curl, noiseSampler, detail, curlField, world, velocity, detail.timeB,
        detail.offsetB), 0.0);
    const float value = detail.weightA * valueA + (1.0 - detail.weightA) * valueB;
    return lerp(1.0, value * detail.meanInverse, detail.strength);
}

// AIR.4F F4.4.2: wisps. The filament mask (tools/GenerateMistFilaments.py) holds sparse strands drawn along the curl field's
// streamlines; it rides the air with the detail's two flow phases and multiplies the detail factor by lerp(1, mask / mean, strength).
// Each phase moves the mask by a fraction of its cycle offset that grows with the local speed (smoothstep from 0 to 1 m/s): in still air
// both phases read the same strands, so they stay crisp instead of a blend of two patterns; in wind the phases differ and a restart
// shows other strands. The offsets are a small part of the tile (WispOffsetShare), so a change of speed across space only bends strands.
struct AirMistWisps
{
    float strength;
    // 1 / the mask tile's size in metres, and 1 / the mask's mean.
    float inverseScale;
    float meanInverse;
    // AIR.4F F4.4.2e: 1 / the mean of the mask's square.
    float squareMeanInverse;
};

static const float WispOffsetShare = 0.12;
static const float WispOffsetSpeed = 1.0;

// The two phases' mask samples, crossfaded: x the mask, y its square (each phase squared before the crossfade, so it averages
// mean(mask^2) at every point of the cycle).
float2 AirMistWispValues(Texture3D<float> filaments, SamplerState noiseSampler, AirMistDetail detail, AirMistWisps wisps, float3 world,
    float3 velocity)
{
    const float offsetWeight = WispOffsetShare * smoothstep(0.0, WispOffsetSpeed, length(velocity));
    const float valueA = filaments.SampleLevel(noiseSampler, (world - velocity * detail.timeA) * wisps.inverseScale +
        detail.offsetA * offsetWeight, 0.0);
    const float valueB = filaments.SampleLevel(noiseSampler, (world - velocity * detail.timeB) * wisps.inverseScale +
        detail.offsetB * offsetWeight, 0.0);
    // The detail's own linear crossfade. (F4.4.2's sharper smoothstep(0.25, 0.75) crossfade held each pattern for 0.5 s and then
    // replaced it within 0.5 s, which showed as abrupt switches in wind; F4.4.2a removed it.)
    const float value = detail.weightA * valueA + (1.0 - detail.weightA) * valueB;
    const float square = detail.weightA * valueA * valueA + (1.0 - detail.weightA) * valueB * valueB;
    return float2(value, square);
}

float AirMistWispFactor(Texture3D<float> filaments, SamplerState noiseSampler, AirMistDetail detail, AirMistWisps wisps, float3 world,
    float3 velocity)
{
    const float value = AirMistWispValues(filaments, noiseSampler, detail, wisps, world, velocity).x;
    return lerp(1.0, value * wisps.meanInverse, wisps.strength);
}

// AIR.4F F4.4.2e: density-dependent detail, after Nubis 3's profile-blended noise (high frequency where thin, broad where dense).
// thickness is n = 1 - exp(-density x extinction x 1 m), the opacity of a metre of the simulated mist (0 to 1), and strength s (0 to 1).
// Thin mist sharpens the strands toward mask^2 / mean(mask^2); dense mist weakens them and strengthens the broad Worley detail. Every
// factor still averages 1 over the textures, so the mist's mean density is unchanged.
static const float DensityDetailWorleyGain = 0.5;
static const float DensityDetailReferenceLength = 1.0;

float AirMistDensityDetailThickness(float density, float extinction)
{
    return 1.0 - exp(-density * extinction * DensityDetailReferenceLength);
}

float AirMistDensityDetailStrength(float detailStrength, float thickness, float strength)
{
    return min(1.0, detailStrength * (1.0 + DensityDetailWorleyGain * strength * thickness));
}

float AirMistDensityWispFactor(Texture3D<float> filaments, SamplerState noiseSampler, AirMistDetail detail, AirMistWisps wisps, float3 world,
    float3 velocity, float thickness, float strength)
{
    const float2 values = AirMistWispValues(filaments, noiseSampler, detail, wisps, world, velocity);
    const float value = lerp(values.x * wisps.meanInverse, values.y * wisps.squareMeanInverse, strength * (1.0 - thickness));
    return lerp(1.0, value, wisps.strength * (1.0 - strength * thickness));
}

// `position` in cell units from the window minimum.
float4 AirRenderMist(AirPathField path, AirMistField mist, float3 position)
{
    return AirRenderMistAndVelocity(path, mist, position, false).mist;
}

#endif
