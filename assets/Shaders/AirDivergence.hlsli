// AIR.4E.2: each fluid piece's divergence, as its net outflow over the AIR.4E.1 face connections (Air.hlsli, AirApertureOf): the
// discretization the pressure projection will use (Documentation/Air/AirArchitecture.md, AIR.4E.0 section 2 and AIR.4E.2).
//
// For piece p of a cell, over its six faces (A: the face's aperture in sub-cell faces / 16; outward positive):
//   outflow_p = sum over faces of +-A * u_f
//   - a face toward a piece q of a neighbour: u_f = (u_p + u_q) / 2 along the axis; one term per linked piece (up to two per side);
//   - a face on the window's edge (open boundary, pressure 0): u_f = (u_p + ambient) / 2, A = p's open sub-faces there;
//   - a closed face (solid, absent, or a cell's other piece) has A = 0 and contributes nothing.
// The result is in m/s times a face's area fraction; divided by the cell size it is the divergence in 1/s of a whole cell's volume.
//
// Two measures, kept apart because the projection's collocated velocity update is approximate (AIR.4E.0 section 2):
//   - collocated: u_f from the pieces' stored velocities alone, the velocity the solver and the mist actually consume;
//   - face: the pressure operator's face velocity, u_f - (phi_upper - phi_lower) (outside phi = 0), with phi one float per piece. Its
//     outflow is collocated + K phi (K: the aperture-weighted graph Laplacian), the projection's residual. Without a pressure buffer
//     phi = 0 and the two measures are equal.

#ifndef AIR_DIVERGENCE_HLSLI
#define AIR_DIVERGENCE_HLSLI

#include "Air.hlsli"

// No pressure buffer: phi = 0 everywhere.
static const uint AirNoPressure = 0xFFFFFFFFu;
// Sub-cell faces per cell face.
static const float AirApertureScale = 1.0 / 16.0;

// AIR.4F.1b: temperature-driven buoyancy, applied by the pressure projection as a force on vertical faces (Documentation/Air/
// AirBuoyancyImplementation4F1b.md). A cell-centred force cannot be balanced by this collocated projection, so the face that carries
// the pressure gradient carries the force too: the right-hand side's face velocity and the correction's face term both add the same
// AirFaceBuoyancy, which makes air at rest in any vertical temperature profile stay at rest.
struct AirBuoyancy
{
    // gravity * timeStep in m/s: the velocity a face gains in one step at the acceleration limit (|a| = g). 0: no buoyancy.
    float impulse;
    // T0, the ambient the field's ΔT is measured from, in kelvin (223.15 to 373.15).
    float referenceTemperature;
};

AirBuoyancy AirNoBuoyancy()
{
    AirBuoyancy buoyancy;
    buoyancy.impulse = 0.0;
    buoyancy.referenceTemperature = 293.15;
    return buoyancy;
}

// The upward velocity a vertical face gains in one step from the ΔT of the pieces below and above it (outside the window: 0, the
// ambient). Ideal gas at constant pressure with the projection's constant density: a = g (ρ0 - ρ) / ρ0 = g ΔT / (T0 + ΔT), from the
// face's mean ΔT, limited to free fall (|a| <= g; it binds only below T0 / 2). An out-of-range ΔT is bounded first and a non-finite one
// reads as the ambient, so T0 + ΔT >= 23.15 K and the result is always finite.
float AirFaceBuoyancy(AirBuoyancy buoyancy, float lowerTemperature, float upperTemperature)
{
    const float mean = 0.5 * (lowerTemperature + upperTemperature);
    const float temperature = isfinite(mean) ? clamp(mean, AirMinimumTemperature, AirMaximumTemperature) : 0.0;
    const float ratio = temperature / (buoyancy.referenceTemperature + temperature);
    return buoyancy.impulse * clamp(ratio, -1.0, 1.0);
}

struct AirDivergenceInputs
{
    AirFieldBinding field;
    // AIR.4E.1 face connections, read-only.
    uint apertureIndex;
    // A read-only raw buffer of one float2 per slot (phi of piece 0, piece 1), or AirNoPressure.
    uint pressureIndex;
    // The velocity outside the window.
    float3 ambient;
    // AIR.4F.1b: the projection's right-hand side only; AirNoBuoyancy for every view of the stored velocity.
    AirBuoyancy buoyancy;
};

// Each piece's two measures: (face, collocated) for piece 0 in xy and piece 1 in zw; an absent piece reads 0.
struct AirPieceDivergence
{
    float2 piece0;
    float2 piece1;
};

uint AirDivergenceSlotIndex(AirFieldBinding field, int3 cell)
{
    const uint3 slot = AirSlotOf(field, cell);
    return slot.x + field.dimensions.x * (slot.y + field.dimensions.y * slot.z);
}

float3 AirDivergenceVelocity(AirFieldBinding field, uint slotIndex, uint piece)
{
    ByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? field.velocityIndex : field.velocityPiece1Index)];
    return asfloat(velocity.Load3(slotIndex * 16));
}

// AIR.4F.1b: a piece's ΔT (velocity.w).
float AirDivergenceTemperature(AirFieldBinding field, uint slotIndex, uint piece)
{
    ByteAddressBuffer velocity = ResourceDescriptorHeap[NonUniformResourceIndex(piece == 0 ? field.velocityIndex : field.velocityPiece1Index)];
    return asfloat(velocity.Load(slotIndex * 16 + 12));
}

float2 AirDivergencePressure(AirDivergenceInputs inputs, uint slotIndex)
{
    if(inputs.pressureIndex == AirNoPressure)
    {
        return float2(0.0, 0.0);
    }

    ByteAddressBuffer pressure = ResourceDescriptorHeap[NonUniformResourceIndex(inputs.pressureIndex)];
    return asfloat(pressure.Load2(slotIndex * 8));
}

uint2 AirDivergenceRecord(AirDivergenceInputs inputs, uint slotIndex)
{
    ByteAddressBuffer apertures = ResourceDescriptorHeap[NonUniformResourceIndex(inputs.apertureIndex)];
    return apertures.Load2(slotIndex * AirApertureBytesPerSlot);
}

// Adds one face's outward flux to `outflow` (face, collocated): `area` in sub-cell faces, `direction` +1 for the +axis face and -1 for
// the -axis face, `faceVelocity` the mean velocity along the axis and `correction` phi(+axis side) - phi(-axis side).
void AirAddFace(inout float2 outflow, uint area, float direction, float faceVelocity, float correction)
{
    const float weight = float(area) * AirApertureScale * direction;
    outflow += weight * float2(faceVelocity - correction, faceVelocity);
}

// AIR.4F.1b: whether `axis` carries buoyancy (vertical faces, buoyancy on).
bool AirBuoyantAxis(AirDivergenceInputs inputs, uint axis)
{
    return axis == 1 && inputs.buoyancy.impulse != 0.0;
}

// A face velocity with the face's buoyancy added. A zero force adds nothing, so a field at the ambient keeps every bit (a -0.0 face
// velocity stays -0.0).
float AirAddBuoyancy(AirDivergenceInputs inputs, float faceVelocity, float lowerTemperature, float upperTemperature)
{
    const float force = AirFaceBuoyancy(inputs.buoyancy, lowerTemperature, upperTemperature);
    return force != 0.0 ? faceVelocity + force : faceVelocity;
}

// AIR P1/C1: one face's share of AirPieceFlux's gross magnitude: A ((|u_own| + |u_other|) / 2 + |F|) along the face's axis.
float AirFaceGross(uint area, float ownVelocity, float otherVelocity, float force)
{
    return float(area) * AirApertureScale * (0.5 * (abs(ownVelocity) + abs(otherVelocity)) + abs(force));
}

// AIR P1/C1 (AirRelativeScaleGaugeG1.md): a piece's outflow (face, collocated) and the gross magnitude of its collocated face terms,
// G = sum over faces of A ((|u_own| + |u_other|) / 2 + |F|) (A: the face's aperture / 16, F: its buoyancy), the scale of the fp32 rounding
// the collocated outflow carries. The outflow's arithmetic is AirPieceOutflow's, operation for operation.
struct AirPieceFlux
{
    float2 outflow;
    float gross;
};

// Piece `piece` (mask `mask`) of `cell`: its outflow (face, collocated) and gross magnitude. `velocity`, `temperature` (its ΔT, read only
// with buoyancy) and `phi` are the piece's own.
AirPieceFlux AirPieceFluxOf(AirDivergenceInputs inputs, int3 cell, uint piece, uint64_t mask, float3 velocity, float temperature, float phi,
    uint2 ownRecord)
{
    float2 outflow = float2(0.0, 0.0);
    float gross = 0.0;

    [unroll]
    for(uint axis = 0; axis < 3; ++axis)
    {
        const bool buoyant = AirBuoyantAxis(inputs, axis);

        // +axis: this cell's own record.
        if(AirHighBoundary(ownRecord, axis))
        {
            float faceVelocity = 0.5 * (velocity[axis] + inputs.ambient[axis]);
            float force = 0.0;

            if(buoyant)
            {
                faceVelocity = AirAddBuoyancy(inputs, faceVelocity, temperature, 0.0);
                force = AirFaceBuoyancy(inputs.buoyancy, temperature, 0.0);
            }

            const uint area = AirApertureOf(ownRecord, axis, piece, 0);
            AirAddFace(outflow, area, 1.0, faceVelocity, -phi);
            gross += AirFaceGross(area, velocity[axis], inputs.ambient[axis], force);
        }
        else
        {
            int3 next = cell;
            next[axis] += 1;
            const uint nextSlot = AirDivergenceSlotIndex(inputs.field, next);
            const float2 nextPhi = AirDivergencePressure(inputs, nextSlot);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(ownRecord, axis, piece, other);

                if(area != 0)
                {
                    const float neighbour = AirDivergenceVelocity(inputs.field, nextSlot, other)[axis];
                    float faceVelocity = 0.5 * (velocity[axis] + neighbour);
                    float force = 0.0;

                    if(buoyant)
                    {
                        const float upperTemperature = AirDivergenceTemperature(inputs.field, nextSlot, other);
                        faceVelocity = AirAddBuoyancy(inputs, faceVelocity, temperature, upperTemperature);
                        force = AirFaceBuoyancy(inputs.buoyancy, temperature, upperTemperature);
                    }

                    AirAddFace(outflow, area, 1.0, faceVelocity, nextPhi[other] - phi);
                    gross += AirFaceGross(area, velocity[axis], neighbour, force);
                }
            }
        }

        // -axis: the window's low edge from the mask, otherwise the -axis neighbour's record with the roles swapped.
        int3 previous = cell;
        previous[axis] -= 1;

        if(!AirContains(inputs.field, previous))
        {
            float faceVelocity = 0.5 * (velocity[axis] + inputs.ambient[axis]);
            float force = 0.0;

            if(buoyant)
            {
                faceVelocity = AirAddBuoyancy(inputs, faceVelocity, 0.0, temperature);
                force = AirFaceBuoyancy(inputs.buoyancy, 0.0, temperature);
            }

            const uint area = AirLowBoundaryAperture(mask, axis);
            AirAddFace(outflow, area, -1.0, faceVelocity, phi);
            gross += AirFaceGross(area, velocity[axis], inputs.ambient[axis], force);
        }
        else
        {
            const uint previousSlot = AirDivergenceSlotIndex(inputs.field, previous);
            const uint2 previousRecord = AirDivergenceRecord(inputs, previousSlot);
            const float2 previousPhi = AirDivergencePressure(inputs, previousSlot);

            [unroll]
            for(uint other = 0; other < 2; ++other)
            {
                const uint area = AirApertureOf(previousRecord, axis, other, piece);

                if(area != 0)
                {
                    const float neighbour = AirDivergenceVelocity(inputs.field, previousSlot, other)[axis];
                    float faceVelocity = 0.5 * (velocity[axis] + neighbour);
                    float force = 0.0;

                    if(buoyant)
                    {
                        const float lowerTemperature = AirDivergenceTemperature(inputs.field, previousSlot, other);
                        faceVelocity = AirAddBuoyancy(inputs, faceVelocity, lowerTemperature, temperature);
                        force = AirFaceBuoyancy(inputs.buoyancy, lowerTemperature, temperature);
                    }

                    AirAddFace(outflow, area, -1.0, faceVelocity, phi - previousPhi[other]);
                    gross += AirFaceGross(area, velocity[axis], neighbour, force);
                }
            }
        }
    }

    AirPieceFlux flux;
    flux.outflow = outflow;
    flux.gross = gross;
    return flux;
}

// Piece `piece` (mask `mask`) of `cell`: its outflow (face, collocated). `velocity`, `temperature` (its ΔT, read only with buoyancy) and
// `phi` are the piece's own.
float2 AirPieceOutflow(AirDivergenceInputs inputs, int3 cell, uint piece, uint64_t mask, float3 velocity, float temperature, float phi,
    uint2 ownRecord)
{
    return AirPieceFluxOf(inputs, cell, piece, mask, velocity, temperature, phi, ownRecord).outflow;
}

// Both pieces of `cell`; absent pieces read 0.
AirPieceDivergence AirCellDivergence(AirDivergenceInputs inputs, int3 cell)
{
    AirPieceDivergence result;
    result.piece0 = float2(0.0, 0.0);
    result.piece1 = float2(0.0, 0.0);

    const AirPieces pieces = AirPiecesOf(inputs.field, cell);
    const uint slotIndex = AirDivergenceSlotIndex(inputs.field, cell);
    const uint2 ownRecord = AirDivergenceRecord(inputs, slotIndex);
    const float2 phi = AirDivergencePressure(inputs, slotIndex);

    if(pieces.piece0 != 0)
    {
        result.piece0 = AirPieceOutflow(inputs, cell, 0, pieces.piece0, AirDivergenceVelocity(inputs.field, slotIndex, 0), 0.0, phi.x,
            ownRecord);
    }

    if(pieces.piece1 != 0)
    {
        result.piece1 = AirPieceOutflow(inputs, cell, 1, pieces.piece1, AirDivergenceVelocity(inputs.field, slotIndex, 1), 0.0, phi.y,
            ownRecord);
    }

    return result;
}

#endif
