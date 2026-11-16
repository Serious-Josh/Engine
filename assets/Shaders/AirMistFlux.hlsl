// AIR.4F.2d.4 Stage 2: conservative mist transport (Documentation/Air/AirConcentratedPointJets4F2d4.md, Stages 2a to 2c). The default
// for projected steps (AirMistSettings::conservativeTransport); runs only in a projected step, between the pressure solve and CSCorrect.
// Steps without the projection keep the advective transport (AirMist.hlsl).
//
// Each fluid piece i holds mass m_i = rho_i V_i (V_i its fluid volume: its sub-cells x (cell / 4)^3). Mist moves only across the AIR.4E.1
// face connections, as one volume flux per face per step:
//   F = dt cell^2 (a / 16) d (u_f - c),   outward from the piece evaluating it,
// with a the face's open sub-faces, d = +1 for the piece's +axis face and -1 for its -axis face, and u_f - c the projection's own face
// velocity: u_f the right-hand side's face velocity (the mean of the two pieces' provisional, pre-correction velocities along the axis,
// plus the face's buoyancy; at the window's edge the mean with the ambient) and c = phi(+axis side) - phi(-axis side) (phi 0 outside).
// This is AirDivergence.hlsli's face measure (AirPieceOutflow, AirAddFace) term for term, so the faces are the ones the projection made
// divergence-free. Both pieces of a face evaluate the same operations on the same values in the same order (a + b = b + a, the
// buoyancy from (lower, upper), the same phi difference), so their fluxes are exact negatives: one transfer per face, no atomics.
//
// The donor's mist crosses a face: outgoing value g_i = s_i rho_i (premultiplied colour and density together), with the cap
// s_i = min(1, V_i / O_i), O_i the piece's total outgoing volume per step, so no piece sends more than it holds (positivity at any
// Courant number; 1 wherever O_i <= V_i). Then
//   m_i' = rho_i V_i - O_i g_i + sum over inflowing faces |F| g_j,   rho_i' = f m_i' / V_i   (f the dissipation factor).
// When the cap binds, O_i g_i = V_i rho_i exactly in arithmetic, so the retained part is 0 by definition rather than a rounding
// residue. Inflow through the window's faces is clear air. Closed faces (a = 0: solid, absent, a cell's other piece) carry nothing.
//
// Two passes over every slot:
//   - CSMistFluxOut: O_i and s_i from the faces; writes g_i into the spare mist buffers (the ones the advective step would write);
//   - CSMistFluxUpdate: m_i' from its own mist (current buffers), its own g_i and its inflowing neighbours' g_j (spare buffers);
//     writes rho_i' into the current buffers in place. Each thread writes only its own entry there and reads neighbours only from the
//     spare buffers, which this pass does not write.
// Absent pieces are written as zero in both, as the advective step does.
//
// AIR.4F.2d.4 Stage 2b.1: adaptive transport substeps. Before the transport, in the same projected step and on the same faces:
//   - CSMistCourant: every piece's outgoing Courant number C_i = O_i / V_i over the full step (O_i at dt), over every piece of the window
//     (not only pieces holding mist, so no advancing front can be missed); each thread group's largest into the substep state;
//   - CSMistSubsteps (one group): the largest over the groups C, then N = min(4, max(1, ceil(C))) (or the diagnostic forced count),
//     dt / N, and the indirect dispatch arguments of substeps 2 to 4 (empty beyond N). Stage 2c.4: a strict threshold of 1, so no
//     substep carries a Courant number above 1 and the cap binds only beyond 4 substeps (Stages 2b.2 to 2c.3b measured the
//     alternatives: a 1.05 tolerance lags sustained flow by up to 4.8%, and mist-aware selection saved no substep).
// Both reductions are fixed trees over wave maxima: no atomics, and the result does not depend on thread order.
// Then N substeps of CSMistFluxOut and CSMistFluxUpdate, each moving dt / N on the same faces (the provisional velocity and phi do not
// change until CSCorrect). Substep 1 is always dispatched; 2 to 4 are indirect. Every pass reads N and dt / N from the substep state,
// so all of them use the same count. Dissipation (f per step) is applied once, in substep N's update: after all of the step's transport,
// as the single-step transport and the advective step apply it. The cap stays in every substep.

#include "AirDivergence.hlsli"

cbuffer AirMistFluxConstants : register(b0)
{
    // The step's provisional (pre-correction) velocity and temperature, the pieces: read-only.
    AirFieldBinding field;
    uint apertureIndex;
    // The solve's phi, a float2 per slot, through its writable view (the solver's storage stays writable).
    uint phiIndex;
    uint mistCurrent0;
    uint mistCurrent1;
    uint mistOutgoing0;
    uint mistOutgoing1;
    float timeStep;
    float dissipationFactor;
    float3 ambient;
    // AIR.4F.1b: the right-hand side's buoyancy (0: none).
    float buoyancyImpulse;
    float referenceTemperature;
    // Stage 2b.1: the pass's substep (1 to 4; 0 for the Courant and substep passes), the substep state's writable view, and the
    // diagnostic forced count (0: adaptive).
    uint substep;
    uint substepStateIndex;
    uint forcedSubsteps;
    // Stage 2b.1: the writable view of the indirect dispatch records of substeps 2 to 4 (CSMistSubsteps writes them).
    uint substepArgumentsIndex;
    // AIR.4F.3 F2b.2: the moving-boundary effective-volume offset S and wall source D (float2 per slot, sub-cell volumes; AirFluxGeometric:
    // none, every piece's volume is its sub-cells, exactly as before).
    uint effectiveOffsetIndex;
    uint wallSourceIndex;
    uint substepPadding2;
};

static const uint AirFluxGeometric = 0xFFFFFFFFu;

// Stage 2b.1: the substep state: N, dt / N and the largest Courant number (bytes 0 to 11), then one float per thread group of
// CSMistCourant from byte SubstepGroupsOffset (its largest Courant number). Matches AirField's substep state size.
static const uint SubstepGroupsOffset = 16;
static const uint MaximumSubsteps = 4;
// The transport passes' 8 x 8 x 8 groups; the Courant pass uses the same grid.
static const uint GroupEdge = 8;

// The step a transport pass moves: dt, or dt / N in a substep (set at each kernel's start).
static float FluxStep;

AirDivergenceInputs FluxInputs()
{
    AirDivergenceInputs inputs;
    inputs.field = field;
    inputs.apertureIndex = apertureIndex;
    inputs.pressureIndex = AirNoPressure;
    inputs.ambient = ambient;
    inputs.buoyancy.impulse = buoyancyImpulse;
    inputs.buoyancy.referenceTemperature = referenceTemperature;
    return inputs;
}

float2 FluxPhi(uint slotIndex)
{
    RWByteAddressBuffer phi = ResourceDescriptorHeap[NonUniformResourceIndex(phiIndex)];
    return asfloat(phi.Load2(slotIndex * 8));
}

float4 LoadMist(uint index0, uint index1, uint slotIndex, uint piece)
{
    RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? index0 : index1)];
    return asfloat(mist.Load4(slotIndex * 16));
}

void StoreMist(uint index0, uint index1, uint slotIndex, uint piece, float4 value)
{
    RWByteAddressBuffer mist = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? index0 : index1)];
    mist.Store4(slotIndex * 16, asuint(value));
}

// What a pass gathers over a piece's faces: the outgoing volume (both passes) and, in the update pass, the mass flowing in.
struct FluxSums
{
    float outgoing;
    float4 incoming;
};

// One face's outward volume per step, F = dt cell^2 (a / 16) d (u_f - c) (see the file comment), added to `sums`; with `gather`, an
// inflow (F < 0) from a neighbour piece adds |F| times that piece's outgoing value. `neighbourSlot` is ~0u at the window's edge.
void AddFace(inout FluxSums sums, bool gather, uint area, float direction, float faceVelocity, float correction, uint neighbourSlot,
    uint neighbourPiece)
{
    const float weight = float(area) * AirApertureScale * direction;
    const float volume = weight * (faceVelocity - correction) * (field.cellSize * field.cellSize * FluxStep);

    if(volume > 0.0)
    {
        sums.outgoing += volume;
    }
    else if(gather && volume < 0.0 && neighbourSlot != ~0u)
    {
        sums.incoming += -volume * LoadMist(mistOutgoing0, mistOutgoing1, neighbourSlot, neighbourPiece);
    }
}

// Piece `piece` (mask `mask`) of `cell`, with its provisional velocity, ΔT and phi: its faces, exactly as AirPieceOutflow walks them.
FluxSums PieceFaces(int3 cell, uint piece, uint64_t mask, float3 velocity, float temperature, float phi, uint2 ownRecord, bool gather)
{
    const AirDivergenceInputs inputs = FluxInputs();
    FluxSums sums;
    sums.outgoing = 0.0;
    sums.incoming = float4(0.0, 0.0, 0.0, 0.0);

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        const bool buoyant = AirBuoyantAxis(inputs, axis);

        // +axis: this cell's own record.
        if(AirHighBoundary(ownRecord, axis))
        {
            float faceVelocity = 0.5 * (velocity[axis] + inputs.ambient[axis]);

            if(buoyant)
            {
                faceVelocity = AirAddBuoyancy(inputs, faceVelocity, temperature, 0.0);
            }

            AddFace(sums, gather, AirApertureOf(ownRecord, axis, piece, 0), 1.0, faceVelocity, -phi, ~0u, 0);
        }
        else
        {
            int3 next = cell;
            next[axis] += 1;
            const uint nextSlot = AirDivergenceSlotIndex(field, next);
            const float2 nextPhi = FluxPhi(nextSlot);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(ownRecord, axis, piece, other);

                if(area != 0)
                {
                    const float neighbour = AirDivergenceVelocity(field, nextSlot, other)[axis];
                    float faceVelocity = 0.5 * (velocity[axis] + neighbour);

                    if(buoyant)
                    {
                        faceVelocity = AirAddBuoyancy(inputs, faceVelocity, temperature, AirDivergenceTemperature(field, nextSlot, other));
                    }

                    AddFace(sums, gather, area, 1.0, faceVelocity, nextPhi[other] - phi, nextSlot, other);
                }
            }
        }

        // -axis: the window's low edge from the mask, otherwise the -axis neighbour's record with the roles swapped.
        int3 previous = cell;
        previous[axis] -= 1;

        if(!AirContains(field, previous))
        {
            float faceVelocity = 0.5 * (velocity[axis] + inputs.ambient[axis]);

            if(buoyant)
            {
                faceVelocity = AirAddBuoyancy(inputs, faceVelocity, 0.0, temperature);
            }

            AddFace(sums, gather, AirLowBoundaryAperture(mask, axis), -1.0, faceVelocity, phi, ~0u, 0);
        }
        else
        {
            const uint previousSlot = AirDivergenceSlotIndex(field, previous);
            const uint2 previousRecord = AirDivergenceRecord(inputs, previousSlot);
            const float2 previousPhi = FluxPhi(previousSlot);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(previousRecord, axis, other, piece);

                if(area != 0)
                {
                    const float neighbour = AirDivergenceVelocity(field, previousSlot, other)[axis];
                    float faceVelocity = 0.5 * (velocity[axis] + neighbour);

                    if(buoyant)
                    {
                        faceVelocity = AirAddBuoyancy(inputs, faceVelocity, AirDivergenceTemperature(field, previousSlot, other), temperature);
                    }

                    AddFace(sums, gather, area, -1.0, faceVelocity, phi - previousPhi[other], previousSlot, other);
                }
            }
        }
    }

    return sums;
}

// A piece's fluid volume, m^3.
float PieceVolume(uint64_t mask)
{
    const float subcell = 0.25 * field.cellSize;
    return float(AirSubcellCount(mask)) * subcell * subcell * subcell;
}

// AIR.4F.3 F2b.2: a piece's effective (control) volume, m^3, at `fraction` of the step (0: its start, 1: its end). The anchored offset S
// gives the end-of-step volume G - S = C(new pose); the wall source D is what the step's open faces carry away, so the volume runs linearly
// from C(anchor) = G - S + D to C(new pose), D / N per substep: the prescribed geometric conservation law, never accumulated beyond the
// step. Without the buffers: the geometric volume.
float EffectiveVolume(uint slotIndex, uint piece, uint64_t mask, float fraction)
{
    if(effectiveOffsetIndex == AirFluxGeometric)
    {
        return PieceVolume(mask);
    }

    RWByteAddressBuffer offsets = ResourceDescriptorHeap[NonUniformResourceIndex(effectiveOffsetIndex)];
    RWByteAddressBuffer sources = ResourceDescriptorHeap[NonUniformResourceIndex(wallSourceIndex)];
    const float offset = asfloat(offsets.Load(slotIndex * 8 + piece * 4));
    const float source = asfloat(sources.Load(slotIndex * 8 + piece * 4));
    const float subcell = 0.25 * field.cellSize;
    const float volume = float(AirSubcellCount(mask)) - offset + source * (1.0 - fraction);
    return max(volume, 0.0) * subcell * subcell * subcell;
}

FluxSums SlotPieceFaces(int3 cell, uint slotIndex, uint piece, uint64_t mask, bool gather)
{
    const uint2 record = AirDivergenceRecord(FluxInputs(), slotIndex);
    const float2 phi = FluxPhi(slotIndex);
    const float temperature = buoyancyImpulse != 0.0 ? AirDivergenceTemperature(field, slotIndex, piece) : 0.0;
    return PieceFaces(cell, piece, mask, AirDivergenceVelocity(field, slotIndex, piece), temperature, phi[piece], record, gather);
}

RWByteAddressBuffer SubstepState()
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(substepStateIndex)];
}

// A transport pass's substep: false when the step takes fewer substeps (its indirect dispatch is then empty anyway). Sets FluxStep.
bool BeginSubstep(out uint count)
{
    count = SubstepState().Load(0);
    FluxStep = asfloat(SubstepState().Load(4));
    return substep <= count;
}

// The largest of `value` over a group of `threads` threads (index `threadIndex`), through wave maxima: every thread must call it.
groupshared float GroupMaxima[64];

float GroupMaximum(float value, uint threadIndex, uint threads)
{
    const uint lanes = WaveGetLaneCount();
    const float waveMaximum = WaveActiveMax(value);

    if(WaveIsFirstLane())
    {
        GroupMaxima[threadIndex / lanes] = waveMaximum;
    }

    GroupMemoryBarrierWithGroupSync();
    float result = 0.0;

    for(uint wave = 0; wave < (threads + lanes - 1) / lanes; ++wave)
    {
        result = max(result, GroupMaxima[wave]);
    }

    return result;
}

uint3 CourantGroups()
{
    return (field.dimensions + GroupEdge - 1) / GroupEdge;
}

[numthreads(8, 8, 8)]
void CSMistCourant(uint3 slot : SV_DispatchThreadID, uint3 group : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    FluxStep = timeStep;
    float largest = 0.0;

    if(all(slot < field.dimensions))
    {
        const int3 cell = AirCellOfSlot(field, slot);
        const uint slotIndex = AirDivergenceSlotIndex(field, cell);
        const AirPieces pieces = AirPiecesOf(field, cell);

        [unroll]
        for(uint piece = 0; piece < 2; ++piece)
        {
            const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;

            if(mask != 0)
            {
                const FluxSums sums = SlotPieceFaces(cell, slotIndex, piece, mask, false);
                // F2b.2: against the smaller of the step's start and end volumes (a shrinking piece's Courant number grows over the step).
                const float volume = min(EffectiveVolume(slotIndex, piece, mask, 0.0), EffectiveVolume(slotIndex, piece, mask, 1.0));
                const float courant = sums.outgoing / volume;
                // A non-finite velocity must not hide the rest: it counts as nothing here (and the cap still bounds its transfers).
                largest = isfinite(courant) ? max(largest, courant) : largest;
            }
        }
    }

    const float groupLargest = GroupMaximum(largest, threadIndex, GroupEdge * GroupEdge * GroupEdge);

    if(threadIndex == 0)
    {
        const uint3 groups = CourantGroups();
        const uint groupIndex = group.x + groups.x * (group.y + groups.y * group.z);
        SubstepState().Store(SubstepGroupsOffset + groupIndex * 4, asuint(groupLargest));
    }
}

static const uint SubstepThreads = 256;

[numthreads(SubstepThreads, 1, 1)]
void CSMistSubsteps(uint threadIndex : SV_GroupIndex)
{
    const uint3 groups = CourantGroups();
    const uint groupCount = groups.x * groups.y * groups.z;
    float largest = 0.0;

    for(uint index = threadIndex; index < groupCount; index += SubstepThreads)
    {
        largest = max(largest, asfloat(SubstepState().Load(SubstepGroupsOffset + index * 4)));
    }

    const float courant = GroupMaximum(largest, threadIndex, SubstepThreads);

    if(threadIndex != 0)
    {
        return;
    }

    const uint adaptive = clamp(uint(ceil(courant)), 1u, MaximumSubsteps);
    const uint count = forcedSubsteps != 0 ? clamp(forcedSubsteps, 1u, MaximumSubsteps) : adaptive;
    SubstepState().Store3(0, uint3(count, asuint(timeStep / float(count)), asuint(courant)));

    // Substeps 2 to 4: one dispatch record each (the out and update passes share it), empty beyond the count.
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[NonUniformResourceIndex(substepArgumentsIndex)];

    for(uint optional = 2; optional <= MaximumSubsteps; ++optional)
    {
        arguments.Store3((optional - 2) * 12, optional <= count ? groups : uint3(0, 0, 0));
    }
}

[numthreads(8, 8, 8)]
void CSMistFluxOut(uint3 slot : SV_DispatchThreadID)
{
    uint count;

    if(any(slot >= field.dimensions) || !BeginSubstep(count))
    {
        return;
    }

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = AirDivergenceSlotIndex(field, cell);
    const AirPieces pieces = AirPiecesOf(field, cell);

    [unroll]
    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;
        float4 outgoing = float4(0.0, 0.0, 0.0, 0.0);

        if(mask != 0)
        {
            // F2b.2: the volume the piece holds at this substep's start.
            const float volume = EffectiveVolume(slotIndex, piece, mask, float(substep - 1) / float(count));
            const FluxSums sums = SlotPieceFaces(cell, slotIndex, piece, mask, false);
            const float share = sums.outgoing > volume ? volume / sums.outgoing : 1.0;
            outgoing = share * LoadMist(mistCurrent0, mistCurrent1, slotIndex, piece);
        }

        StoreMist(mistOutgoing0, mistOutgoing1, slotIndex, piece, outgoing);
    }
}

[numthreads(8, 8, 8)]
void CSMistFluxUpdate(uint3 slot : SV_DispatchThreadID)
{
    uint count;

    if(any(slot >= field.dimensions) || !BeginSubstep(count))
    {
        return;
    }

    // The step's dissipation, once: in the last substep's update, after all of the step's transport.
    const float dissipation = substep == count ? dissipationFactor : 1.0;

    const int3 cell = AirCellOfSlot(field, slot);
    const uint slotIndex = AirDivergenceSlotIndex(field, cell);
    const AirPieces pieces = AirPiecesOf(field, cell);

    [unroll]
    for(uint piece = 0; piece < 2; ++piece)
    {
        const uint64_t mask = piece == 0 ? pieces.piece0 : pieces.piece1;
        float4 updated = float4(0.0, 0.0, 0.0, 0.0);

        if(mask != 0 && effectiveOffsetIndex != AirFluxGeometric)
        {
            // F2b.2: mass and volume advance together. m' = rho V(start) - g O + inflow; V(end) = V(start) - D / N (the wall's share of the
            // step); rho' = m' / V(end). When the projection makes the open faces carry exactly D / N net, a uniform density stays uniform.
            const float start = EffectiveVolume(slotIndex, piece, mask, float(substep - 1) / float(count));
            const float end = EffectiveVolume(slotIndex, piece, mask, float(substep) / float(count));
            const FluxSums sums = SlotPieceFaces(cell, slotIndex, piece, mask, true);
            const bool capped = sums.outgoing > start;
            const float4 own = LoadMist(mistCurrent0, mistCurrent1, slotIndex, piece);
            const float4 outgoing = LoadMist(mistOutgoing0, mistOutgoing1, slotIndex, piece);
            const float4 retained = capped ? float4(0.0, 0.0, 0.0, 0.0) : own * start - outgoing * sums.outgoing;
            updated = end > 0.0 ? (retained + sums.incoming) / end * dissipation : float4(0.0, 0.0, 0.0, 0.0);
        }
        else if(mask != 0)
        {
            const float volume = PieceVolume(mask);
            const FluxSums sums = SlotPieceFaces(cell, slotIndex, piece, mask, true);
            // The cap's own test (as CSMistFluxOut): when it binds the piece sends everything it holds, so nothing is retained.
            // Stage 2c.5: in density form, rho' = (rho - g O / V + inflow / V) f, the same equation as (rho V - g O + inflow) f / V, so
            // a piece with no transfer keeps its value exactly (still mist stays bit-identical, AIR.4F.2a); O <= V keeps it non-negative.
            const bool capped = sums.outgoing > volume;
            const float4 own = LoadMist(mistCurrent0, mistCurrent1, slotIndex, piece);
            const float4 outgoing = LoadMist(mistOutgoing0, mistOutgoing1, slotIndex, piece);
            const float4 retained = capped ? float4(0.0, 0.0, 0.0, 0.0) : own - outgoing * (sums.outgoing / volume);
            updated = (retained + sums.incoming / volume) * dissipation;
        }

        StoreMist(mistCurrent0, mistCurrent1, slotIndex, piece, updated);
    }
}
